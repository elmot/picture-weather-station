#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "nvs_flash.h"
#include "weather-station.h"
#include "datastreams.h"
#include "hw_support.h"
#include "chart.h"
#include "fox_condition.h"
#include "sensor_history.h"

extern "C" {
void sensor_task(void*);
void ruuvi_task_init(void);
}
void wifi_task(void*);

/*-----------------------------------------------------------------------
 * Mutex-protected shared data (SensorHistory<T,1> = single-slot)
 *---------------------------------------------------------------------*/
SensorHistory<ruuvi_data_t, 1> g_ruuvi_history{};
SensorHistory<adafruit_data_t, 1> g_adafruit_history{};
SensorHistory<chart_data_t, 1> g_chart_history{};

extern "C" void pushRuuviData(const ruuvi_data_t* data)
{
    g_ruuvi_history.push(*data);
}

static const char* TAG = "picture-ws";

bool isDataExpired(const TickType_t last_update)
{
    const auto now = xTaskGetTickCount();
    if (last_update == 0) return true;
    return pdTICKS_TO_MS(now - last_update) > (1000 * 60 * 60);
}

/*-----------------------------------------------------------------------
 * AHT20 indoor sensor
 *---------------------------------------------------------------------*/

typedef struct
{
    float temperature;
    float humidity;
} aht20_reading_t;

SensorHistory<aht20_reading_t, 96> g_aht20_history{};

extern "C" void pushAht20Data(float temperature, float humidity)
{
    g_aht20_history.push({temperature, humidity});
}


/*-----------------------------------------------------------------------
 * app_main
 *---------------------------------------------------------------------*/
extern "C" void app_main(void)
{
    /* NVS -- required by Wi-Fi and BLE */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    hw_init();
    /* Initialise Slint platform — line-by-line rendering (no frame buffers) */
    slint_esp_init(SlintPlatformConfiguration<slint::platform::Rgb565Pixel>{
        .size = slint::PhysicalSize({LCD_H_RES, LCD_V_RES}),
        .panel_handle = s_panel,
        .touch_handle = nullptr,
        .byte_swap = true,
    });
    backlight(true);
    g_meteo_queue = xQueueCreate(1, sizeof(meteo_data_t));
    ruuvi_task_init();
    xTaskCreate(wifi_task, "weather", 16384, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "Weather task started (lat=%s, lon=%s)", CONFIG_PWS_LAT, CONFIG_PWS_LON);
    xTaskCreate(sensor_task, "sensor", 8096, nullptr, 5, nullptr);

    auto ui = WeatherStation::create();
    ChartSupportCode<WeatherStation, ChartSupport>(ui).setup();

    /* Periodic timer: read shared data and push into Slint properties */
    slint::Timer timer(std::chrono::milliseconds(1000),
                       [&ui]()
                       {
                           /* Fetched meteo data — read atomically from queue */
                           meteo_data_t meteo{};
                           if (xQueuePeek(g_meteo_queue, &meteo, 0) == pdTRUE)
                           {
                               ui->set_weather_code(static_cast<int>(meteo.code));
                               ui->set_day(static_cast<bool>(meteo.is_day));
                               ui->set_meteo_data(OpenMeteoData{
                                   .tempC = meteo.temp,
                                   .tempFeelsC = meteo.feels,
                                   .windSpeed = meteo.wind_speed,
                                   .windGusts = meteo.wind_gusts,
                                   .windDir = meteo.wind_dir,
                                   .condition = fox_condition(meteo.code),
                                   .connFail = isDataExpired(meteo.last_update),
                               });
                           }
                           else
                           {
                               static const OpenMeteoData default_meteo_data{
                                   .tempC = 0.0f, .tempFeelsC = 0.0f, .windSpeed = 0.0f, .windGusts = 0.0f,
                                   .windDir = slint::SharedString(""), .condition = FoxConditionEnum::Rain,
                                   .connFail = true,
                               };
                               ui->set_meteo_data(default_meteo_data);
                           }

                           /* Indoor sensor */
                           auto data = g_aht20_history.map([](const aht20_reading_t& reading)
                           {
                               return reading.temperature;
                           });
                           const auto tempHistory = std::make_shared<slint::VectorModel<float>>(data);
                           const auto last = g_aht20_history.last();
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

                           /* Adafruit IO */
                           {
                               auto aio = g_adafruit_history.last();
                               ui->set_adafruit_data({
                                   .value = aio.value,
                                   .connFail = isDataExpired(aio.last_update)
                               });
                           }

                           /* AIO chart */
                           {
                               auto chart = g_chart_history.last();
                               std::vector<float> cv(chart.values,
                                                     chart.values + chart.count);
                               ui->set_chart_history(
                                   std::make_shared<slint::VectorModel<float>>(cv));
                           }
                       });

    ESP_LOGI(TAG, "Starting Slint UI");
    ui->run();
}
