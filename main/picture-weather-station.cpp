#include <cstring>
#include <cmath>
#include <cstdio>
#include <png.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "nvs_flash.h"
#include "slint-esp.h"
#include "weather-station.h"
#include "datastreams.h"
#include "hw_support.h"

extern "C" {
void sensor_task(void*);
void ruuvi_task_init(void);
void wifi_task(void*);
}

static const char* TAG = "picture-ws";

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

    hw_init();
    /* Initialise Slint platform — line-by-line rendering (no frame buffers) */
    slint_esp_init(SlintPlatformConfiguration<slint::platform::Rgb565Pixel> {
        .size = slint::PhysicalSize({ LCD_H_RES, LCD_V_RES }),
        .panel_handle = s_panel,
        .touch_handle = nullptr,
        .byte_swap = true,
    });
    backlight(true);
    ruuvi_task_init();
    xTaskCreate(wifi_task, "weather", 8192, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "Weather task started (lat=%s, lon=%s)", CONFIG_PWS_LAT, CONFIG_PWS_LON);
    xTaskCreate(sensor_task, "sensor", 4096, nullptr, 5, nullptr);

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
