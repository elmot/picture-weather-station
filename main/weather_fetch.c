#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "weather";

//#define FETCH_INTERVAL_MS  (15 * 60 * 1000)   /* 15 min */
#define FETCH_INTERVAL_MS  (5 * 1000)   /* 15 min */
#define MAX_RESPONSE_SIZE  4096

/* TODO: replace stub URL with real weather API endpoint */
#define WEATHER_HOST "https://api.open-meteo.com"
#define WEATHER_URL WEATHER_HOST\
"/v1/forecast?latitude=" CONFIG_PWS_LAT \
"&longitude=" CONFIG_PWS_LON \
"&models=metno_seamless&current=temperature_2m,relative_humidity_2m,apparent_temperature,snowfall,showers,rain,is_day,precipitation,weather_code,cloud_cover,pressure_msl,surface_pressure,wind_gusts_10m,wind_direction_10m,wind_speed_10m&wind_speed_unit=ms"

/*-----------------------------------------------------------------------
 * HTTP event handler — accumulates response body into user_data buffer
 *---------------------------------------------------------------------*/
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *resp = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t avail = resp->cap - resp->len - 1;
        size_t copy = evt->data_len < avail ? evt->data_len : avail;
        if (copy > 0) {
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
static const char * wind_names [8] ={"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
static void weather_fetch_and_parse(void)
{
    static char buf [MAX_RESPONSE_SIZE];

    http_buf_t resp = { .buf = buf, .len = 0, .cap = MAX_RESPONSE_SIZE };

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

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return;
    }

    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        return;
    }

    ESP_LOGI(TAG, "Received %d bytes from %s", (int)resp.len, WEATHER_HOST);

    cJSON *root = cJSON_Parse(resp.buf);

    if (!root) {
        ESP_LOGE(TAG, "JSON parse error");
        return;
    }

    const cJSON * current = cJSON_GetObjectItem(root, "current");
    const int wind_dir = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_direction_10m"));
    const unsigned int wind_sector = ((wind_dir + 22) % 360 / 45) % 8;
    const char* wind_dir_name = wind_names[wind_sector];
    ESP_LOGI(TAG, "Weather Code: %d", (int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "weather_code")));
    ESP_LOGI(TAG, "Temperature: %.1f C", cJSON_GetNumberValue(cJSON_GetObjectItem(current, "temperature_2m")));
    ESP_LOGI(TAG, "Humidity: %.0f%%", cJSON_GetNumberValue(cJSON_GetObjectItem(current, "relative_humidity_2m")));
    ESP_LOGI(TAG, "Wind: %s  %.1f m/s (gusts: %.1f m/s)",
             wind_dir_name,
             cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_speed_10m")),
             cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_gusts_10m")));
    cJSON_Delete(root);
}

/*-----------------------------------------------------------------------
 * FreeRTOS task — fetches weather periodically
 *---------------------------------------------------------------------*/
_Noreturn static void weather_task(void *arg)
{
    (void)arg;
    for (;;) {
        weather_fetch_and_parse();
        vTaskDelay(pdMS_TO_TICKS(FETCH_INTERVAL_MS));
    }
}

void weather_task_init(void)
{
    xTaskCreate(weather_task, "weather", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "Weather task started (lat=%s, lon=%s)", CONFIG_PWS_LAT, CONFIG_PWS_LON);
}
