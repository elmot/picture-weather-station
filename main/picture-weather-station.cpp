#include <cmath>
#include <cstdio>
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

    /* Periodic timer: poll volatile globals and push data into Slint properties */
    slint::Timer timer(std::chrono::milliseconds(1000),
        [&ui]()
        {
            /* Weather icon (code + day/night handled by Slint conditionals) */
            ui->set_weather_code((int)g_meteo.code);
            ui->set_is_day((bool)g_meteo.is_day);

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

            /* Adafruit IO */
            if (!std::isnan(g_adafruit.value))
            {
                snprintf(buf, sizeof(buf), "%.1f", g_adafruit.value);
                ui->set_adafruit_text(slint::SharedString(buf));
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
