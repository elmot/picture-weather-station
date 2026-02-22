#include <cstring>
#include <cmath>
#include <cstdio>
#include <png.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "nvs_flash.h"
#include "slint-esp.h"
#include "weather-station.h"

extern "C" {
#include "datastreams.h"
void sensor_task(void*);
void wifi_init_sta(void);
void ruuvi_task_init(void);
void weather_task_init(void);
}

static const char* TAG = "picture-ws";

static_assert(sizeof(CONFIG_PWS_WIFI_SSID) > 1, "WiFi SSID can not be empty. "
              "Define CONFIG_PWS_WIFI_SSID via `idf.py menuconfig` or in `skdconfig.secrets` file");

static_assert(sizeof(CONFIG_PWS_WIFI_PASSWORD) > 1, "WiFi password can not be empty. "
              "Define CONFIG_PWS_WIFI_PASSWORD via `idf.py menuconfig` or in `skdconfig.secrets` file");

/*-----------------------------------------------------------------------
 * Unihiker K10 LCD wiring (ILI9341, 240x320)
 *---------------------------------------------------------------------*/
#define LCD_SPI_HOST     SPI2_HOST
#define PIN_LCD_MOSI     21
#define PIN_LCD_CLK      12
#define PIN_LCD_CS       GPIO_NUM_14
#define PIN_LCD_DC       GPIO_NUM_13

/*-----------------------------------------------------------------------
 * I2C bus
 *---------------------------------------------------------------------*/
#define I2C_SDA          GPIO_NUM_47
#define I2C_SCL          GPIO_NUM_48
#define I2C_CLK_HZ       (400 * 1000)

i2c_master_bus_handle_t s_i2c_bus;

/*-----------------------------------------------------------------------
 * XL9535 I2C port extender
 *---------------------------------------------------------------------*/
#define XL9535_ADDR      0x20
#define XL9535_REG_OUT0  0x02
#define XL9535_REG_OUT1  0x03
#define XL9535_REG_CFG0  0x06
#define XL9535_REG_CFG1  0x07

static i2c_master_dev_handle_t s_xl9535;

#define LCD_H_RES        240
#define LCD_V_RES        320
#define LCD_CLK_HZ       (40 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel;

/*-----------------------------------------------------------------------
 * Initialise SPI bus + ILI9341 panel, turn on backlight
 *---------------------------------------------------------------------*/
static void lcd_init()
{
    
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = PIN_LCD_CLK,
        .data2_io_num = GPIO_NUM_NC,
        .data3_io_num = GPIO_NUM_NC,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .data_io_default_level = false,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
        .flags = {},
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_CLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .flags = {},
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t dev_cfg = {
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .reset_gpio_num = GPIO_NUM_NC,
        .vendor_config = nullptr,
        .flags = {}
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &dev_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* dir=2 in MicroPython -> 180 deg rotation -> mirror both axes */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));

    ESP_LOGI(TAG, "LCD initialised (%dx%d)", LCD_H_RES, LCD_V_RES);
}

/*-----------------------------------------------------------------------
 * Initialise I2C master bus
 *---------------------------------------------------------------------*/
static void i2c_init()
{
    static i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = true,.allow_pd = 0},
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    ESP_LOGI(TAG, "I2C initialised (SDA=%d, SCL=%d)", I2C_SDA, I2C_SCL);
}

/*-----------------------------------------------------------------------
 * Initialise XL9535 port extender: pin 0 -> output high
 *---------------------------------------------------------------------*/
static void xl9535_init()
{
    constexpr  i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9535_ADDR,
        .scl_speed_hz = I2C_CLK_HZ,
        .scl_wait_us = 0,
        .flags = {},
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_xl9535));

    uint8_t cfg_cmd[] = {XL9535_REG_CFG0, 0xFE};
    ESP_ERROR_CHECK(i2c_master_transmit(s_xl9535, cfg_cmd, sizeof(cfg_cmd), -1));

    uint8_t out_cmd[] = {XL9535_REG_OUT0, 0x01};
    ESP_ERROR_CHECK(i2c_master_transmit(s_xl9535, out_cmd, sizeof(out_cmd), -1));

    ESP_LOGI(TAG, "XL9535 pin 0 -> output HIGH");
}

/*-----------------------------------------------------------------------
 * Custom read callback for libpng to read from memory
 *---------------------------------------------------------------------*/
typedef struct
{
    const uint8_t* data;
    size_t offset;
} png_mem_read_t;

static void png_mem_read_fn(png_structp png, png_bytep out, size_t count)
{
    auto* state = static_cast<png_mem_read_t*>(png_get_io_ptr(png));
    memcpy(out, state->data + state->offset, count);
    state->offset += count;
}

/*-----------------------------------------------------------------------
 * Decode embedded PNG into a slint::Image (RGBA8).
 *---------------------------------------------------------------------*/
static slint::Image decode_png_to_slint_image(const uint8_t* data)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    assert(png);
    png_infop info = png_create_info_struct(png);
    assert(info);

    png_mem_read_t state = {.data = data, .offset = 0};
    png_set_read_fn(png, &state, png_mem_read_fn);
    png_read_info(png, info);

    const uint32_t w = png_get_image_width(png, info);
    const uint32_t h = png_get_image_height(png, info);
    const png_byte color_type = png_get_color_type(png, info);
    const png_byte bit_depth = png_get_bit_depth(png, info);

    ESP_LOGI(TAG, "PNG: %ux%u, color_type=%d, bit_depth=%d", w, h,
             color_type, bit_depth);

    /* Normalize to 8-bit RGBA */
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (!(color_type & PNG_COLOR_MASK_ALPHA))
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    png_read_update_info(png, info);

    slint::SharedPixelBuffer<slint::Rgba8Pixel> pixel_buffer(w, h);
    auto* pixels = pixel_buffer.begin();

    auto* row_buf = static_cast<png_bytep>(malloc(png_get_rowbytes(png, info)));
    assert(row_buf);

    for (uint32_t y = 0; y < h; y++)
    {
        png_read_row(png, row_buf, nullptr);
        for (uint32_t x = 0; x < w; x++)
        {
            pixels[y * w + x] = slint::Rgba8Pixel{
                row_buf[x * 4 + 0],
                row_buf[x * 4 + 1],
                row_buf[x * 4 + 2],
                row_buf[x * 4 + 3],
            };
        }
    }

    free(row_buf);
    png_destroy_read_struct(&png, &info, nullptr);

    ESP_LOGI(TAG, "PNG decoded to Slint image (%ux%u)", w, h);
    return {pixel_buffer};
}

/*-----------------------------------------------------------------------
 * app_main
 *---------------------------------------------------------------------*/
extern "C" void app_main(void)
{
    /* NVS -- required by WiFi and BLE */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    ruuvi_task_init();
    weather_task_init();

    i2c_init();
    xl9535_init();

    xTaskCreate(sensor_task, "sensor", 4096, nullptr, 5, nullptr);

    lcd_init();

    /* Initialise Slint platform — line-by-line rendering (no frame buffers) */
    slint_esp_init(SlintPlatformConfiguration<slint::platform::Rgb565Pixel> {
        .size = slint::PhysicalSize({ LCD_H_RES, LCD_V_RES }),
        .panel_handle = s_panel,
        .touch_handle = nullptr,
        .byte_swap = true,
    });

    auto ui = WeatherStation::create();

    char* shown_icon_ptr = nullptr;

    /* Periodic timer: poll volatile globals and push data into Slint properties */
    slint::Timer timer(std::chrono::milliseconds(1000),
        [&ui, &shown_icon_ptr]()
        {
            /* Weather icon */
            if (g_meteo.icon_png != nullptr && shown_icon_ptr != g_meteo.icon_png)
            {
                shown_icon_ptr = g_meteo.icon_png;
                auto icon = decode_png_to_slint_image(
                    reinterpret_cast<const uint8_t*>(g_meteo.icon_png));
                ui->set_weather_icon(icon);
                ESP_LOGI(TAG, "Weather icon updated");
            }

            char buf[64];

            /* Outdoor weather */
            if (!std::isnan(g_meteo.temp))
            {
                snprintf(buf, sizeof(buf), "%.1f\xc2\xb0""C (feels %.1f\xc2\xb0""C) %.0f%%",
                         g_meteo.temp, g_meteo.feels, g_meteo.humidity);
                ui->set_outdoor_text(slint::SharedString(buf));
            }

            /* Wind */
            if (!std::isnan(g_meteo.wind_speed))
            {
                snprintf(buf, sizeof(buf), "Wind: %s %.1f (%.1f) m/s",
                         g_meteo.wind_dir, g_meteo.wind_speed, g_meteo.wind_gusts);
                ui->set_wind_text(slint::SharedString(buf));
            }

            /* Pressure */
            if (!std::isnan(g_meteo.pressure))
            {
                snprintf(buf, sizeof(buf), "Pressure: %.0f hPa", g_meteo.pressure);
                ui->set_pressure_text(slint::SharedString(buf));
            }

            /* Indoor sensor */
            if (!std::isnan(g_aht20.temperature))
            {
                snprintf(buf, sizeof(buf), "Indoor: %.1f\xc2\xb0""C  %.0f%%",
                         g_aht20.temperature, g_aht20.humidity);
                ui->set_indoor_text(slint::SharedString(buf));
            }

            /* Ruuvi tag */
            if (ruuvi_last_update != 0)
            {
                snprintf(buf, sizeof(buf), "Ruuvi: %.1f\xc2\xb0""C %.0f%% %.0fmm %.2fV",
                         g_ruuvi_data.temperature, g_ruuvi_data.humidity,
                         g_ruuvi_data.pressure_mmhg, g_ruuvi_data.battery_voltage);
                ui->set_ruuvi_text(slint::SharedString(buf));
            }

            /* Data age status line */
            const TickType_t now = xTaskGetTickCount();
            const int web_age_s = g_meteo.last_update
                                      ? static_cast<int>((now - g_meteo.last_update) / configTICK_RATE_HZ)
                                      : -1;
            const int ruuvi_age_s = ruuvi_last_update
                                        ? static_cast<int>((now - ruuvi_last_update) / configTICK_RATE_HZ)
                                        : -1;
            if (web_age_s >= 0 || ruuvi_age_s >= 0)
            {
                snprintf(buf, sizeof(buf), "Last upd: Web %dm, Ruuvi %dm",
                         web_age_s / 60, ruuvi_age_s / 60);
                ui->set_status_text(slint::SharedString(buf));
            }
        });

    ESP_LOGI(TAG, "Starting Slint UI");
    ui->run();
}
