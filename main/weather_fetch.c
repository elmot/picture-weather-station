#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "datastreams.h"

static const char* TAG = "weather";
volatile meteo_data_t g_meteo = {
    .temp = NAN, .feels = NAN, .humidity = NAN,
    .wind_speed = NAN, .wind_gusts = NAN, .wind_dir = "",
    .pressure = NAN, .code = -1, .icon_png = nullptr, .last_update = 0,
};

//#define FETCH_INTERVAL_MS (15 * 60 * 1000) /* 15 min */ //todo restore
#define FETCH_INTERVAL_MS  (5 * 1000)   /* 15 min */
#define MAX_RESPONSE_SIZE  4096

#define WEATHER_HOST "https://api.open-meteo.com"
#define WEATHER_URL WEATHER_HOST\
"/v1/forecast?latitude=" CONFIG_PWS_LAT \
"&longitude=" CONFIG_PWS_LON \
"&models=metno_seamless&current=temperature_2m,relative_humidity_2m,apparent_temperature,snowfall,showers,rain,is_day,precipitation,weather_code,cloud_cover,pressure_msl,surface_pressure,wind_gusts_10m,wind_direction_10m,wind_speed_10m&wind_speed_unit=ms"

struct icon_info
{
    const int code;
    const void* day;
    const void* night;
};
extern const uint8_t _binary_wmo_icon_00d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_00n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_01d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_01n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_02d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_02n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_03d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_03n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_45d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_45n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_53d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_53n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_57d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_57n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_61d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_61n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_63d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_63n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_65d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_65n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_66d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_66n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_67d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_67n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_71d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_71n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_73d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_73n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_75d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_75n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_80d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_80n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_81d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_81n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_84d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_84n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_85d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_85n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_86d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_86n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_95d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_95n_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_96d_png_start; // NOLINT(*-reserved-identifier)
extern const uint8_t _binary_wmo_icon_96n_png_start; // NOLINT(*-reserved-identifier)

static const struct icon_info icon_info_table[] = {
  {.code=00, .day=&_binary_wmo_icon_00d_png_start , .night= &_binary_wmo_icon_00n_png_start},
  {.code=01, .day=&_binary_wmo_icon_01d_png_start , .night= &_binary_wmo_icon_01n_png_start},
  {.code=02, .day=&_binary_wmo_icon_02d_png_start , .night= &_binary_wmo_icon_02n_png_start},
  {.code=03, .day=&_binary_wmo_icon_03d_png_start , .night= &_binary_wmo_icon_03n_png_start},
  {.code=45, .day=&_binary_wmo_icon_45d_png_start , .night= &_binary_wmo_icon_45n_png_start},
  {.code=53, .day=&_binary_wmo_icon_53d_png_start , .night= &_binary_wmo_icon_53n_png_start},
  {.code=57, .day=&_binary_wmo_icon_57d_png_start , .night= &_binary_wmo_icon_57n_png_start},
  {.code=61, .day=&_binary_wmo_icon_61d_png_start , .night= &_binary_wmo_icon_61n_png_start},
  {.code=63, .day=&_binary_wmo_icon_63d_png_start , .night= &_binary_wmo_icon_63n_png_start},
  {.code=65, .day=&_binary_wmo_icon_65d_png_start , .night= &_binary_wmo_icon_65n_png_start},
  {.code=66, .day=&_binary_wmo_icon_66d_png_start , .night= &_binary_wmo_icon_66n_png_start},
  {.code=67, .day=&_binary_wmo_icon_67d_png_start , .night= &_binary_wmo_icon_67n_png_start},
  {.code=71, .day=&_binary_wmo_icon_71d_png_start , .night= &_binary_wmo_icon_71n_png_start},
  {.code=73, .day=&_binary_wmo_icon_73d_png_start , .night= &_binary_wmo_icon_73n_png_start},
  {.code=75, .day=&_binary_wmo_icon_75d_png_start , .night= &_binary_wmo_icon_75n_png_start},
  {.code=80, .day=&_binary_wmo_icon_80d_png_start , .night= &_binary_wmo_icon_80n_png_start},
  {.code=81, .day=&_binary_wmo_icon_81d_png_start , .night= &_binary_wmo_icon_81n_png_start},
  {.code=84, .day=&_binary_wmo_icon_84d_png_start , .night= &_binary_wmo_icon_84n_png_start},
  {.code=85, .day=&_binary_wmo_icon_85d_png_start , .night= &_binary_wmo_icon_85n_png_start},
  {.code=86, .day=&_binary_wmo_icon_86d_png_start , .night= &_binary_wmo_icon_86n_png_start},
  {.code=89, .day=&_binary_wmo_icon_86n_png_start , .night= &_binary_wmo_icon_86n_png_start},
      
};

/*-----------------------------------------------------------------------
 * Find icon by weather code and day/night status
 *---------------------------------------------------------------------*/
static const void* get_weather_icon(int code, bool is_day)
{
    for (size_t i = 0; i < sizeof(icon_info_table) / sizeof(icon_info_table[0]); i++)
    {
        if (icon_info_table[i].code == code)
        {
            return is_day ? icon_info_table[i].day : icon_info_table[i].night;
        }
    }
    return nullptr; /* Not found */
}

/*-----------------------------------------------------------------------
 * HTTP event handler — accumulates response body into user_data buffer
 *---------------------------------------------------------------------*/
typedef struct
{
    char* buf;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    http_buf_t* resp = (http_buf_t*)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA)
    {
        size_t avail = resp->cap - resp->len - 1;
        size_t copy = evt->data_len < avail ? evt->data_len : avail;
        if (copy > 0)
        {
            memcpy(resp->buf + resp->len, evt->data, copy);
            resp->len += copy;
            resp->buf[resp->len] = '\0';
        }
    }
    return ESP_OK;
}

/*-----------------------------------------------------------------------
 * Fetch JSON from URL and parse with cJSON
 *---------------------------------------------------------------------*/
static const char* wind_names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

static void weather_fetch_and_parse(void)
{
    static char buf[MAX_RESPONSE_SIZE];

    http_buf_t resp = {.buf = buf, .len = 0, .cap = MAX_RESPONSE_SIZE};

    esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return;
    }

    if (status != 200)
    {
        ESP_LOGW(TAG, "HTTP status %d", status);
        return;
    }

    ESP_LOGI(TAG, "Received %d bytes from %s", (int)resp.len, WEATHER_HOST);

    cJSON* root = cJSON_Parse(resp.buf);

    if (!root)
    {
        ESP_LOGE(TAG, "JSON parse error");
        return;
    }

    const cJSON* current = cJSON_GetObjectItem(root, "current");
    const int wind_dir = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_direction_10m"));
    const unsigned int wind_sector = ((wind_dir + 22) % 360 / 45) % 8;
    const char* wind_dir_name = wind_names[wind_sector];
    const bool is_day = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "is_day"));
    g_meteo.code       = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "weather_code"));
    g_meteo.temp       = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "temperature_2m"));
    g_meteo.feels      = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "apparent_temperature"));
    g_meteo.humidity   = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "relative_humidity_2m"));
    g_meteo.wind_speed = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_speed_10m"));
    g_meteo.wind_gusts = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_gusts_10m"));
    g_meteo.wind_dir   = wind_dir_name;
    g_meteo.pressure   = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "pressure_msl"));
    g_meteo.icon_png   = (char*)get_weather_icon(g_meteo.code, is_day);
    g_meteo.last_update = xTaskGetTickCount();
    ESP_LOGI(TAG, "Weather Code: %d", g_meteo.code);
    ESP_LOGI(TAG, "Temperature: %.1f C", g_meteo.temp);
    ESP_LOGI(TAG, "Humidity: %.0f%%", g_meteo.humidity);
    ESP_LOGI(TAG, "Wind: %s  %.1f m/s (gusts: %.1f m/s)",
             wind_dir_name, g_meteo.wind_speed, g_meteo.wind_gusts);
    cJSON_Delete(root);
}

/*-----------------------------------------------------------------------
 * FreeRTOS task — fetches weather periodically
 *---------------------------------------------------------------------*/
_Noreturn static void weather_task(void* arg)
{
    (void)arg;
    for (;;)
    {
        weather_fetch_and_parse();
        vTaskDelay(pdMS_TO_TICKS(FETCH_INTERVAL_MS));
    }
}

void weather_task_init(void)
{
    xTaskCreate(weather_task, "weather", 8192, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "Weather task started (lat=%s, lon=%s)", CONFIG_PWS_LAT, CONFIG_PWS_LON);
}
