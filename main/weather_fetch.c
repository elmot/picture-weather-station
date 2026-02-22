#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "datastreams.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static_assert(sizeof(CONFIG_PWS_WIFI_SSID) > 1, "WiFi SSID can not be empty. "
              "Define CONFIG_PWS_WIFI_SSID via `idf.py menuconfig` or in `skdconfig.secrets` file");

static_assert(sizeof(CONFIG_PWS_WIFI_PASSWORD) > 1, "WiFi password can not be empty. "
              "Define CONFIG_PWS_WIFI_PASSWORD via `idf.py menuconfig` or in `skdconfig.secrets` file");

static const char* TAG = "wifi";
volatile meteo_data_t g_meteo = { // NOLINT(*-interfaces-global-init)
    .temp = NAN, .feels = NAN, .humidity = NAN,
    .wind_speed = NAN, .wind_gusts = NAN, .wind_dir = "",
    .pressure = NAN, .code = -1, .is_day = true, .last_update = 0,
};

//#define FETCH_INTERVAL_MS (15 * 60 * 1000) /* 15 min */ //todo restore
#define FETCH_INTERVAL_MS  (50 * 1000)   /* 15 min */
#define MAX_RESPONSE_SIZE  4096

#define WEATHER_HOST "https://api.open-meteo.com"
#define WEATHER_URL WEATHER_HOST\
"/v1/forecast?latitude=" CONFIG_PWS_LAT \
"&longitude=" CONFIG_PWS_LON \
"&models=metno_seamless&current=temperature_2m,relative_humidity_2m,apparent_temperature,snowfall,showers,rain,is_day,precipitation,weather_code,cloud_cover,pressure_msl,surface_pressure,wind_gusts_10m,wind_direction_10m,wind_speed_10m&wind_speed_unit=ms"

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
    const cJSON* current = cJSON_GetObjectItem(root, "current");

    if (!current)
    {
        ESP_LOGE(TAG, "JSON parse error");

        return;
    }

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
    g_meteo.is_day     = is_day;
    g_meteo.last_update = xTaskGetTickCount();
    ESP_LOGI(TAG, "Weather Code: %d", g_meteo.code);
    ESP_LOGI(TAG, "Temperature: %.1f C", g_meteo.temp);
    ESP_LOGI(TAG, "Humidity: %.0f%%", g_meteo.humidity);
    ESP_LOGI(TAG, "Wind: %s  %.1f m/s (gusts: %.1f m/s)",
             wind_dir_name, g_meteo.wind_speed, g_meteo.wind_gusts);
    cJSON_Delete(root);
}

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying connection %d...", s_retry_num);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = CONFIG_PWS_WIFI_SSID,
            .password = CONFIG_PWS_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", CONFIG_PWS_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to '%s'", CONFIG_PWS_WIFI_SSID);
    } else {
        ESP_LOGW(TAG, "Could not connect to '%s'", CONFIG_PWS_WIFI_SSID);
    }
}

/*-----------------------------------------------------------------------
 * FreeRTOS task — fetches weather periodically
 *---------------------------------------------------------------------*/
_Noreturn void wifi_task(void* arg)
{
    (void)arg;
    wifi_init_sta();
    for (;;)
    {
        weather_fetch_and_parse();
        vTaskDelay(pdMS_TO_TICKS(FETCH_INTERVAL_MS));
    }
}
