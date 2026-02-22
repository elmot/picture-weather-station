#include <string.h>
#include <math.h>
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
#include "lvgl.h"
#include "datastreams.h"

char* shown_icon_ptr = nullptr;

static lv_obj_t* internet_weather_icon_obj = nullptr;
static lv_color_t* internet_weather_icon_pixels = nullptr;
static lv_obj_t* lbl_outdoor = nullptr;
static lv_obj_t* lbl_wind = nullptr;
static lv_obj_t* lbl_pressure = nullptr;
static lv_obj_t* lbl_indoor = nullptr;
static lv_obj_t* lbl_ruuvi = nullptr;
static lv_obj_t* lbl_status = nullptr;
static_assert(sizeof(CONFIG_PWS_WIFI_SSID) > 1, "WiFi SSID can not be empty. "
              "Define CONFIG_PWS_WIFI_SSID via `idf.py menuconfig` or in `skdconfig.secrets` file");

static_assert(sizeof(CONFIG_PWS_WIFI_PASSWORD) > 1, "WiFi password can not be empty. "
              "Define CONFIG_PWS_WIFI_PASSWORD via `idf.py menuconfig` or in `skdconfig.secrets` file");

static const char* TAG = "picture-ws";
/*-----------------------------------------------------------------------
 * Unihiker K10 LCD wiring (ILI9341, 240x320)
 * Adjust these defines if your board revision differs.
 *---------------------------------------------------------------------*/
#define LCD_SPI_HOST     SPI2_HOST
#define PIN_LCD_MOSI     21
#define PIN_LCD_CLK      12
#define PIN_LCD_CS       14
#define PIN_LCD_DC       13

/*-----------------------------------------------------------------------
 * I2C bus
 *---------------------------------------------------------------------*/
#define I2C_SDA          47
#define I2C_SCL          48
#define I2C_CLK_HZ       (400 * 1000)

i2c_master_bus_handle_t s_i2c_bus;

/*-----------------------------------------------------------------------
 * XL9535 I2C port extender
 *---------------------------------------------------------------------*/
#define XL9535_ADDR      0x20
#define XL9535_REG_OUT0  0x02   /* Output Port 0 */
#define XL9535_REG_OUT1  0x03   /* Output Port 1 */
#define XL9535_REG_CFG0  0x06   /* Configuration Port 0 (1=input, 0=output) */
#define XL9535_REG_CFG1  0x07   /* Configuration Port 1 */

static i2c_master_dev_handle_t s_xl9535;

#define LCD_H_RES        240
#define LCD_V_RES        320
#define LCD_CLK_HZ       (40 * 1000 * 1000)

static esp_lcd_panel_handle_t s_panel;

/*-----------------------------------------------------------------------
 * PNG image embedded in flash (see EMBED_FILES in CMakeLists.txt)
 *---------------------------------------------------------------------*/
extern const uint8_t _binary_wind_fox_a_png_start; // NOLINT(*-reserved-identifier)


/*-----------------------------------------------------------------------
 * SPI transfer-done callback — signals LVGL that the buffer is free
 *---------------------------------------------------------------------*/
static lv_disp_drv_t* s_disp_drv;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t* edata,
                                void* user_ctx)
{
    lv_disp_flush_ready(s_disp_drv);
    return false;
}

/*-----------------------------------------------------------------------
 * LVGL display flush callback -- pushes rendered area to the ILI9341
 *---------------------------------------------------------------------*/
// ReSharper disable once CppParameterMayBeConstPtrOrRef
static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map)
{
    esp_lcd_panel_handle_t panel = drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_map);
}

/*-----------------------------------------------------------------------
 * Initialise LVGL: draw buffer, display driver (no tick timer)
 *---------------------------------------------------------------------*/
static void lvgl_init(void)
{
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_color_t* buf = heap_caps_malloc(LCD_H_RES * 40 * sizeof(lv_color_t),
                                       MALLOC_CAP_DMA);
    assert(buf);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LCD_H_RES * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.user_data = s_panel;
    s_disp_drv = &disp_drv;
    lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "LVGL initialised");
}

/*-----------------------------------------------------------------------
 * Force LVGL to render all pending changes to the display
 *---------------------------------------------------------------------*/
void lvgl_render(void)
{
    lv_tick_inc(100);
    lv_refr_now(nullptr);
}

/*-----------------------------------------------------------------------
 * Initialise SPI bus + ILI9341 panel, turn on backlight
 *---------------------------------------------------------------------*/
static void lcd_init(void)
{
    /* SPI bus */
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    /* SPI panel IO */
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io));

    /* ILI9341 panel driver */
    esp_lcd_panel_dev_config_t dev_cfg = {
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &dev_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* dir=2 in MicroPython -> 180 deg rotation -> mirror both axes */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGI(TAG, "LCD initialised (%dx%d)", LCD_H_RES, LCD_V_RES);
}

/*-----------------------------------------------------------------------
 * Initialise I2C master bus
 *---------------------------------------------------------------------*/
static void i2c_init(void)
{
    static i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    ESP_LOGI(TAG, "I2C initialised (SDA=%d, SCL=%d)", I2C_SDA, I2C_SCL);
}

/*-----------------------------------------------------------------------
 * Initialise XL9535 port extender: pin 0 -> output high
 *---------------------------------------------------------------------*/
static void xl9535_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9535_ADDR,
        .scl_speed_hz = I2C_CLK_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_xl9535));

    /* Backlight GPIO */
    /* Set pin 0 of port 0 as output (clear bit 0, default 0xFF = all inputs) */
    uint8_t cfg_cmd[] = {XL9535_REG_CFG0, 0xFE};
    ESP_ERROR_CHECK(i2c_master_transmit(s_xl9535, cfg_cmd, sizeof(cfg_cmd), -1));

    /* Drive pin 0 high */
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
    png_mem_read_t* state = (png_mem_read_t*)png_get_io_ptr(png);
    memcpy(out, state->data + state->offset, count);
    state->offset += count;
}

/*-----------------------------------------------------------------------
 * Decode embedded PNG into an LVGL image descriptor (LV_IMG_CF_TRUE_COLOR).
 * The pixel buffer is allocated on PSRAM and must stay alive while the
 * image is displayed by LVGL.
 *---------------------------------------------------------------------*/
static void decode_png_to_lvgl(const uint8_t* data, lv_img_dsc_t* img_dsc)
{
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             nullptr, nullptr, nullptr);
    assert(png);
    png_infop info = png_create_info_struct(png);
    assert(info);

    png_mem_read_t state = {.data = data, .offset = 0};
    png_set_read_fn(png, &state, png_mem_read_fn);
    png_read_info(png, info);

    const unsigned int w = png_get_image_width(png, info);
    const unsigned int h = png_get_image_height(png, info);
    const png_byte color_type = png_get_color_type(png, info);
    const png_byte bit_depth = png_get_bit_depth(png, info);

    ESP_LOGI(TAG, "PNG: %dx%d, color_type=%d, bit_depth=%d", w, h,
             color_type, bit_depth);

    /* Normalize to 8-bit RGB */
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    if (color_type & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(png);

    png_read_update_info(png, info);

    /* Allocate full pixel buffer (prefer PSRAM) */
    if (img_dsc->data != nullptr)
    {
        heap_caps_free((uint8_t*)img_dsc->data);
    }

    img_dsc->data = heap_caps_malloc(w * h * sizeof(lv_color_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const lv_color_t* pixels = (lv_color_t*)img_dsc->data;

    png_bytep row_rgb = malloc(png_get_rowbytes(png, info));
    assert(row_rgb);

    for (int y = 0; y < h; y++)
    {
        png_read_row(png, row_rgb, nullptr);
        for (int x = 0; x < w; x++)
        {
            pixels[y * w + x] = lv_color_make(row_rgb[x * 3 + 0],
                                              row_rgb[x * 3 + 1],
                                              row_rgb[x * 3 + 2]);
        }
    }

    free(row_rgb);
    png_destroy_read_struct(&png, &info, nullptr);

    *img_dsc = (lv_img_dsc_t){
        .header.cf = LV_IMG_CF_TRUE_COLOR,
        .header.always_zero = 0,
        .header.reserved = 0,
        .header.w = w,
        .header.h = h,
        .data_size = w * h * sizeof(lv_color_t),
        .data = (const uint8_t*)pixels,
    };
    ESP_LOGI(TAG, "PNG decoded to LVGL image (%dx%d)", w, h);
}

void sensor_task(void*);
void wifi_init_sta(void);
void ruuvi_task_init(void);
void weather_task_init(void);
/*-----------------------------------------------------------------------
 * app_main
 *---------------------------------------------------------------------*/
_Noreturn void app_main(void)
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
    lvgl_init();

    /* Orange background */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_make(0xFF, 0x7F, 0x00), 0);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);

    /* Decode and display the embedded PNG */
    static lv_img_dsc_t fox_img = {.data = nullptr};
    decode_png_to_lvgl(&_binary_wind_fox_a_png_start, &fox_img);
    lv_obj_t* img = lv_img_create(lv_scr_act());
    lv_img_set_src(img, &fox_img);
    lv_obj_align(img, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* Create weather data labels */
    static lv_style_t style_lbl;
    lv_style_init(&style_lbl);
    lv_style_set_text_color(&style_lbl, lv_color_white());
    lv_style_set_text_font(&style_lbl, &lv_font_montserrat_14);

#define LABEL_X 4
#define LABEL_Y_START 165
#define LABEL_SPACING 18

    lbl_outdoor = lv_label_create(lv_scr_act());
    lv_obj_add_style(lbl_outdoor, &style_lbl, 0);
    lv_obj_set_pos(lbl_outdoor, LABEL_X, LABEL_Y_START);
    lv_label_set_text(lbl_outdoor, "");

    lbl_wind = lv_label_create(lv_scr_act());
    lv_obj_add_style(lbl_wind, &style_lbl, 0);
    lv_obj_set_pos(lbl_wind, LABEL_X, LABEL_Y_START + LABEL_SPACING);
    lv_label_set_text(lbl_wind, "");

    lbl_pressure = lv_label_create(lv_scr_act());
    lv_obj_add_style(lbl_pressure, &style_lbl, 0);
    lv_obj_set_pos(lbl_pressure, LABEL_X, LABEL_Y_START + LABEL_SPACING * 2);
    lv_label_set_text(lbl_pressure, "");

    lbl_indoor = lv_label_create(lv_scr_act());
    lv_obj_add_style(lbl_indoor, &style_lbl, 0);
    lv_obj_set_pos(lbl_indoor, LABEL_X, LABEL_Y_START + LABEL_SPACING * 3);
    lv_label_set_text(lbl_indoor, "");

    lbl_ruuvi = lv_label_create(lv_scr_act());
    lv_obj_add_style(lbl_ruuvi, &style_lbl, 0);
    lv_obj_set_pos(lbl_ruuvi, LABEL_X, LABEL_Y_START + LABEL_SPACING * 4);
    lv_label_set_text(lbl_ruuvi, "");

    lbl_status = lv_label_create(lv_scr_act());
    lv_obj_add_style(lbl_status, &style_lbl, 0);
    lv_obj_set_pos(lbl_status, LABEL_X, LABEL_Y_START + LABEL_SPACING * 5);
    lv_label_set_text(lbl_status, "");

    lvgl_render();

    ESP_LOGI(TAG, "Done");

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool need_render = false;

        /* Check if internet weather icon is available and not yet displayed */
        if (g_meteo.icon_png != nullptr && shown_icon_ptr != g_meteo.icon_png)
        {
            shown_icon_ptr = g_meteo.icon_png;
            const uint8_t* icon_start = (const uint8_t*)g_meteo.icon_png;
            static lv_img_dsc_t internet_weather_icon_img;
            decode_png_to_lvgl(icon_start, &internet_weather_icon_img);
            internet_weather_icon_pixels = (lv_color_t*)internet_weather_icon_img.data;
            internet_weather_icon_obj = lv_img_create(lv_scr_act());
            lv_img_set_src(internet_weather_icon_obj, &internet_weather_icon_img);
            lv_obj_align(internet_weather_icon_obj, LV_ALIGN_TOP_LEFT, 0, 80);
            need_render = true;
            ESP_LOGI(TAG, "Internet weather icon displayed");
        }

        /* Update weather labels */
        if (!isnan(g_meteo.temp))
        {
            lv_label_set_text_fmt(lbl_outdoor, "%.1f\xc2\xb0""C (feels %.1f\xc2\xb0""C) %.0f%%",
                                  g_meteo.temp, g_meteo.feels, g_meteo.humidity);
            need_render = true;
        }
        if (!isnan(g_meteo.wind_speed))
        {
            lv_label_set_text_fmt(lbl_wind, "Wind: %s %.1f (%.1f) m/s",
                                  g_meteo.wind_dir, g_meteo.wind_speed, g_meteo.wind_gusts);
            need_render = true;
        }
        if (!isnan(g_meteo.pressure))
        {
            lv_label_set_text_fmt(lbl_pressure, "Pressure: %.0f hPa",
                                  g_meteo.pressure);
            need_render = true;
        }
        if (!isnan(g_aht20.temperature))
        {
            lv_label_set_text_fmt(lbl_indoor, "Indoor: %.1f\xc2\xb0""C  %.0f%%",
                                  g_aht20.temperature, g_aht20.humidity);
            need_render = true;
        }
        if (ruuvi_last_update != 0)
        {
            lv_label_set_text_fmt(lbl_ruuvi, "Ruuvi: %.1f\xc2\xb0""C %.0f%% %.0fmm %.2fV",
                                  g_ruuvi_data.temperature, g_ruuvi_data.humidity,
                                  g_ruuvi_data.pressure_mmhg, g_ruuvi_data.battery_voltage);
            need_render = true;
        }

        /* Data age status line */
        const TickType_t now = xTaskGetTickCount();
        const int web_age_s = g_meteo.last_update
                                  ? (int)((now - g_meteo.last_update) / configTICK_RATE_HZ)
                                  : -1;
        const int ruuvi_age_s = ruuvi_last_update
                                    ? (int)((now - ruuvi_last_update) / configTICK_RATE_HZ)
                                    : -1;
        if (web_age_s >= 0 || ruuvi_age_s >= 0)
        {
            char buf[48];
            snprintf(buf, sizeof(buf), "Last upd: Web %dm, Ruuvi %dm", web_age_s / 60, ruuvi_age_s / 60);
            lv_label_set_text(lbl_status, buf);
            need_render = true;
        }

        if (need_render)
        {
            lvgl_render();
        }
    }
}
