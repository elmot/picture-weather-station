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
void wifi_task(void*);
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
    ruuvi_task_init();
    xTaskCreate(wifi_task, "weather", 8192, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "Weather task started (lat=%s, lon=%s)", CONFIG_PWS_LAT, CONFIG_PWS_LON);
    xTaskCreate(sensor_task, "sensor", 4096, nullptr, 5, nullptr);

    auto ui = WeatherStation::create();
    ChartSupportCode<WeatherStation, ChartSupport>(ui).setup();

    /* Periodic timer: poll volatile globals and push data into Slint properties */
    slint::Timer timer(std::chrono::milliseconds(1000),
                       [&ui]()
                       {
                           /* Weather icon (code + day/night handled by Slint conditionals) */
                           ui->set_weather_code(static_cast<int>(g_meteo.code));
                           ui->set_day(static_cast<bool>(g_meteo.is_day));
                           /* Fox */
                           /* Fetched meteo data */
                           const auto meteoData = OpenMeteoData{
                               .tempC = g_meteo.temp,
                               .tempFeelsC = g_meteo.feels,
                               .windSpeed = g_meteo.wind_speed,
                               .windGusts = g_meteo.wind_gusts,
                               .windDir = g_meteo.wind_dir,
                               .condition = fox_condition(g_meteo.code),
                               .connFail = isDataExpired(g_meteo.last_update)
                           };
                           ui->set_meteo_data(meteoData);

                           /* Indoor sensor */
                           auto data = g_aht20_history.map([](const aht20_reading_t& reading) { return reading.temperature; });
                           const auto tempHistory = std::make_shared<slint::VectorModel<float>>(data);
                           ui->set_indoor_data(LocalData{
                               .tempC = g_aht20_history.last().temperature,
                               .relHumidity = g_aht20_history.last().humidity,
                               .tempHistory = tempHistory,
                           });

                           /* Ruuvi tag */
                           ui->set_roovi_data(RuuviData{
                               .tempC = g_ruuvi_data.temperature,
                               .atmPHgmm = g_ruuvi_data.pressure_mmhg,
                               .relHumidity = g_ruuvi_data.humidity,
                               .connFail = isDataExpired(g_ruuvi_data.last_update),
                               .battV = g_ruuvi_data.battery_voltage,
                           });

                           /* Adafruit IO */
                           ui->set_adafruit_data({
                               .value = static_cast<float>(g_adafruit.value),
                               .connFail = isDataExpired(g_adafruit.last_update)
                           });

                       });

    ESP_LOGI(TAG, "Starting Slint UI");
    ui->run();
}
