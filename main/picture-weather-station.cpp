#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "weather-station.h"
#include "datastreams.h"
#include "hw_support.h"
#include "power_bsp.h"
#include "chart.h"
#include "fox_condition.h"
#include "sensor_history.h"
#include "rtc_state.h"

extern "C" {
void ruuvi_task_init(void);
bool ruuvi_wait_for_sample(uint32_t timeout_ms);
void shtc3_init(void);
bool shtc3_read_once(float *t, float *h);
void wifi_start(void);
bool wifi_wait_connected(uint32_t timeout_ms);
void wifi_fetch_remote_data(void);
void wifi_publish_ruuvi(void);
}

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t RUUVI_SCAN_TIMEOUT_MS   = 10000;
constexpr uint64_t SLEEP_DURATION_US       = 30ULL * 60ULL * 1000000ULL;

/*-----------------------------------------------------------------------
 * Mutex-protected shared data (SensorHistory<T,1> = single-slot)
 *---------------------------------------------------------------------*/
SensorHistory<ruuvi_data_t, 1> g_ruuvi_history{};
SensorHistory<adafruit_data_t, 1> g_adafruit_history{};
SensorHistory<chart_data_t, 1> g_chart_history{};
SensorHistory<shtc3_reading_t, 96> g_shtc3_history{};

extern "C" void pushRuuviData(const ruuvi_data_t* data)
{
    g_ruuvi_history.push(*data);
}

extern "C" void pushShtc3Data(float temperature, float humidity)
{
    g_shtc3_history.push({temperature, humidity});
}

static const char* TAG = "picture-ws";

bool isDataExpired(const TickType_t last_update)
{
    if (last_update == 0) return true;
    const auto now = xTaskGetTickCount();
    return pdTICKS_TO_MS(now - last_update) > (1000 * 60 * 60);
}

/*-----------------------------------------------------------------------
 * Push collected sensor / web data into Slint properties (one shot).
 *---------------------------------------------------------------------*/
static void populate_ui(const slint::ComponentHandle<WeatherStation>& ui)
{
    /* Open-Meteo */
    meteo_data_t meteo{};
    if (g_meteo_queue && xQueuePeek(g_meteo_queue, &meteo, 0) == pdTRUE)
    {
        ui->set_weather_code(static_cast<int>(meteo.code));
        const bool day = meteo.is_day;
        ui->set_day(day);
        ui->set_meteo_data(OpenMeteoData{
            .tempC = meteo.temp,
            .tempFeelsC = meteo.feels,
            .windSpeed = meteo.wind_speed,
            .windGusts = meteo.wind_gusts,
            .windDir = meteo.wind_dir,
            .condition = fox_condition(meteo.code, day, meteo.wind_speed, meteo.wind_gusts),
            .connFail = isDataExpired(meteo.last_update),
        });
    }
    else
    {
        ui->set_meteo_data(OpenMeteoData{
            .tempC = 0.0f, .tempFeelsC = 0.0f, .windSpeed = 0.0f, .windGusts = 0.0f,
            .windDir = slint::SharedString(""), .condition = FoxConditionEnum::Rainy,
            .connFail = true,
        });
    }

    /* Indoor sensor + chart history (history may include samples restored from RTC) */
    auto data = g_shtc3_history.map([](const shtc3_reading_t& reading)
    {
        return reading.temperature;
    });
    const auto tempHistory = std::make_shared<slint::VectorModel<float>>(data);
    const auto last = g_shtc3_history.last();
    ui->set_indoor_data(LocalData{
        .tempC = last.temperature,
        .relHumidity = last.humidity,
        .tempHistory = tempHistory,
    });

    /* Ruuvi tag */
    {
        auto ruuvi = g_ruuvi_history.last();
        ui->set_roovi_data(RuuviData{
            .tempC = ruuvi.temperature,
            .atmPHgmm = ruuvi.pressure_mmhg,
            .relHumidity = ruuvi.humidity,
            .connFail = isDataExpired(ruuvi.last_update),
            .battV = ruuvi.battery_voltage,
        });
    }

    /* Adafruit IO scalar */
    {
        auto aio = g_adafruit_history.last();
        ui->set_adafruit_data({
            .value = aio.value,
            .connFail = isDataExpired(aio.last_update),
        });
    }

    /* Adafruit IO chart */
    {
        auto chart = g_chart_history.last();
        std::vector<float> cv(chart.values, chart.values + chart.count);
        ui->set_chart_history(std::make_shared<slint::VectorModel<float>>(cv));
    }
}

/*-----------------------------------------------------------------------
 * app_main — one wake/render cycle, then 30-min deep sleep.
 *---------------------------------------------------------------------*/
extern "C" void app_main(void)
{
    power_log_boot_reason();

    /* NVS — required by Wi-Fi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    hw_init();
    power_log_battery();

    /* Restore SHTC3 chart history from RTC slow memory (no-op on cold boot). */
    rtc_history_restore(g_shtc3_history);

    /* Slint platform — line-by-line rendering via the e-paper adapter. */
    slint_esp_init(SlintPlatformConfiguration<slint::platform::Rgb565Pixel>{
        .size = slint::PhysicalSize({LCD_H_RES, LCD_V_RES}),
        .panel_handle = s_panel,
        .touch_handle = nullptr,
        .byte_swap = false,
    });

    g_meteo_queue = xQueueCreate(1, sizeof(meteo_data_t));

    /* Local indoor read — instant, on the existing I2C bus. */
    shtc3_init();
    {
        float t = 0.0f, h = 0.0f;
        if (shtc3_read_once(&t, &h)) pushShtc3Data(t, h);
    }

    /* Kick off Wi-Fi and BLE concurrently — coex on ESP32-S3 handles it. */
    wifi_start();
    ruuvi_task_init();

    /* Wait for Wi-Fi connect, then fetch all remote data first. */
    bool wifi_ok = wifi_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
    if (wifi_ok) wifi_fetch_remote_data();

    /* Ruuvi scan likely produced a sample during the HTTP work above. */
    ruuvi_wait_for_sample(RUUVI_SCAN_TIMEOUT_MS);

    /* Publish Ruuvi values once we have a fresh sample. */
    if (wifi_ok) wifi_publish_ruuvi();

    /* Build UI, push values, render once. */
    auto ui = WeatherStation::create();
    ChartSupportCode<WeatherStation, ChartSupport>(ui).setup();
    populate_ui(ui);

    ESP_LOGI(TAG, "Rendering UI");
    ui->run();   /* exits when epd_panel_adapter calls slint::quit_event_loop() */
    ESP_LOGI(TAG, "Render complete");

    /* Persist chart history and put the panel + SoC to sleep. */
    rtc_history_save(g_shtc3_history);
    epaper_sleep();

    /* Stop Wi-Fi to clear PHY calibration cleanly. NimBLE is left alone —
     * deep sleep powers the controller down and wipes RAM regardless. */
    esp_wifi_stop();
    esp_wifi_deinit();

    power_pre_deep_sleep();

    ESP_LOGI(TAG, "Entering deep sleep for 30 minutes");
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
    esp_deep_sleep_start();
}
