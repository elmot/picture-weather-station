#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "datastreams.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_heap_caps.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static_assert(sizeof(CONFIG_PWS_WIFI_SSID) > 1, "WiFi SSID can not be empty. "
              "Define CONFIG_PWS_WIFI_SSID via `idf.py menuconfig` or in `skdconfig.secrets` file");

static_assert(sizeof(CONFIG_PWS_WIFI_PASSWORD) > 1, "WiFi password can not be empty. "
              "Define CONFIG_PWS_WIFI_PASSWORD via `idf.py menuconfig` or in `skdconfig.secrets` file");

static const char* TAG = "wifi";
QueueHandle_t g_meteo_queue;
volatile adafruit_data_t g_adafruit = {.value = NAN, .last_update = 0};

float g_aio_chart[AIO_CHART_MAX];
volatile int g_aio_chart_count = 0;

#define FETCH_INTERVAL_MS (5 * 60 * 1000) /* 5 min */
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

    const esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return;

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
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
        cJSON_Delete(root);
        return;
    }

    const int wind_dir_deg = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_direction_10m"));
    const unsigned int wind_sector = ((wind_dir_deg + 22) % 360 / 45) % 8;

    meteo_data_t meteo = {
        .temp = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "temperature_2m")),
        .feels = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "apparent_temperature")),
        .humidity = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "relative_humidity_2m")),
        .wind_speed = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_speed_10m")),
        .wind_gusts = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_gusts_10m")),
        .wind_dir = wind_names[wind_sector],
        .pressure = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "pressure_msl")),
        .code = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "weather_code")),
        .is_day = (bool)(int)cJSON_GetNumberValue(cJSON_GetObjectItem(current, "is_day")),
        .last_update = xTaskGetTickCount(),
    };

    cJSON_Delete(root);

    xQueueOverwrite(g_meteo_queue, &meteo);

    ESP_LOGI(TAG, "Weather Code: %d", meteo.code);
    ESP_LOGI(TAG, "Temperature: %.1f C", meteo.temp);
    ESP_LOGI(TAG, "Humidity: %.0f%%", meteo.humidity);
    ESP_LOGI(TAG, "Wind: %s  %.1f m/s (gusts: %.1f m/s)",
             meteo.wind_dir, meteo.wind_speed, meteo.wind_gusts);
}

/*-----------------------------------------------------------------------
 * Fetch latest value from Adafruit IO feed
 *---------------------------------------------------------------------*/
#define AIO_URL "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/feeds/" CONFIG_PWS_ADAFRUIT_IO_CO2_FEED_KEY "/data/last"

static void adafruit_fetch(void)
{
    static char buf[1024];
    http_buf_t resp = {.buf = buf, .len = 0, .cap = sizeof(buf)};

    esp_http_client_config_t config = {
        .url = AIO_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return;
    esp_http_client_set_header(client, "X-AIO-Key", CONFIG_PWS_ADAFRUIT_IO_KEY);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Adafruit IO request failed: %s", esp_err_to_name(err));
        return;
    }
    if (status != 200)
    {
        ESP_LOGW(TAG, "Adafruit IO HTTP status %d", status);
        return;
    }

    cJSON* root = cJSON_Parse(resp.buf);
    if (!root)
    {
        ESP_LOGE(TAG, "Adafruit IO JSON parse error");
        return;
    }

    const char* value_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "value"));
    if (value_str)
    {
        g_adafruit.value = strtof(value_str, nullptr);
        g_adafruit.last_update = xTaskGetTickCount();
        ESP_LOGI(TAG, "Adafruit IO: %s = %.2f", CONFIG_PWS_ADAFRUIT_IO_CO2_FEED_KEY, g_adafruit.value);
    }

    cJSON_Delete(root);
}

/*-----------------------------------------------------------------------
 * Publish Ruuvi sensor data as JSON to Adafruit IO feed
 *---------------------------------------------------------------------*/
#define AIO_PUBLISH_URL "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/feeds/" CONFIG_PWS_ADAFRUIT_IO_PUBLISH_JSON_FEED "/data"

static void adafruit_publish_ruuvi(void)
{
    if (g_ruuvi_data.last_update == 0) return;

    cJSON* value_obj = cJSON_CreateObject();
    if (value_obj == nullptr) return;
    cJSON_AddNumberToObject(value_obj, "temperature", g_ruuvi_data.temperature);
    cJSON_AddNumberToObject(value_obj, "pressure", g_ruuvi_data.pressure_mmhg);
    cJSON_AddNumberToObject(value_obj, "humidity", g_ruuvi_data.humidity);
    char* value_str = cJSON_PrintUnformatted(value_obj);
    cJSON_Delete(value_obj);
    if (value_str == nullptr) return;

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr)
    {
        cJSON_free(value_str);
        return;
    }
    cJSON_AddStringToObject(root, "value", value_str);
    char* body = cJSON_PrintUnformatted(root);
    cJSON_free(value_str);
    cJSON_Delete(root);

    if (!body) return;

    esp_http_client_config_t config = {
        .url = AIO_PUBLISH_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client==nullptr)
    {
        cJSON_free(body);
        return;
    }
    esp_http_client_set_header(client, "X-AIO-Key", CONFIG_PWS_ADAFRUIT_IO_KEY);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(body);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Adafruit IO publish failed: %s", esp_err_to_name(err));
        return;
    }
    if (status != 200 && status != 201)
    {
        ESP_LOGW(TAG, "Adafruit IO publish HTTP status %d", status);
        return;
    }
    ESP_LOGI(TAG, "Published Ruuvi data to Adafruit IO feed '%s'",
             CONFIG_PWS_ADAFRUIT_IO_PUBLISH_JSON_FEED);
}

/*-----------------------------------------------------------------------
 * Publish Ruuvi pressure to a dedicated Adafruit IO feed
 *---------------------------------------------------------------------*/
#define AIO_PRESSURE_URL "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/feeds/" CONFIG_PWS_ADAFRUIT_IO_PUBLISH_PRESSURE_FEED "/data"


/*-----------------------------------------------------------------------
 * Fetch chart data from Adafruit IO
 * Response: { "columns":["date","avg"], "data":[["...", "val"], ...] }
 *---------------------------------------------------------------------*/
#define AIO_CHART_URL "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/feeds/" CONFIG_PWS_ADAFRUIT_IO_CHART_FEED \
    "/data/chart?hours=48&resolution=30&field=avg"

#define AIO_CHART_BUF_SIZE 8192

static void aio_chart_fetch(void)
{
    char* buf = heap_caps_malloc(AIO_CHART_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf)
    {
        ESP_LOGE(TAG, "AIO chart: alloc failed");
        return;
    }

    http_buf_t resp = {.buf = buf, .len = 0, .cap = AIO_CHART_BUF_SIZE};

    esp_http_client_config_t config = {
        .url = AIO_CHART_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr)
    {
        free(buf);
        return;
    }
    esp_http_client_set_header(client, "X-AIO-Key", CONFIG_PWS_ADAFRUIT_IO_KEY);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "AIO chart fetch failed: %s", esp_err_to_name(err));
        free(buf);
        return;
    }
    if (status != 200)
    {
        ESP_LOGW(TAG, "AIO chart HTTP status %d", status);
        free(buf);
        return;
    }

    ESP_LOGI(TAG, "AIO chart: received %d bytes", (int)resp.len);

    cJSON* root = cJSON_Parse(buf);
    if (!root)
    {
        ESP_LOGE(TAG, "AIO chart: JSON parse error");
        free(buf);
        return;
    }

    cJSON* data_arr = cJSON_GetObjectItem(root, "data");
    if (!data_arr || !cJSON_IsArray(data_arr))
    {
        ESP_LOGE(TAG, "AIO chart: missing 'data' array");
        cJSON_Delete(root);
        free(buf);
        return;
    }

    /* Each element is [timestamp_str, value_str] — already chronological */
    int total = 0;
    cJSON const * row;
    cJSON_ArrayForEach(row, data_arr)
    {
        if (total >= AIO_CHART_MAX) break;
        if (!cJSON_IsArray(row)) continue;
        cJSON* val = cJSON_GetArrayItem(row, 1);
        if (!val) continue;
        const char* s = cJSON_GetStringValue(val);
        if (s)
        {
            g_aio_chart[total++] = strtof(s, nullptr);
        }
        else if (cJSON_IsNumber(val))
        {
            g_aio_chart[total++] = (float)cJSON_GetNumberValue(val);
        }
    }

    cJSON_Delete(root);
    free(buf);

    g_aio_chart_count = total;
    ESP_LOGI(TAG, "AIO chart: %d entries", total);
}

static void adafruit_publish_pressure(void)
{
    if (sizeof(CONFIG_PWS_ADAFRUIT_IO_PUBLISH_PRESSURE_FEED) <= 1)
    {
        ESP_LOGW(TAG, "Pressure feed key not configured, skipping publish");
        return;
    }
    if (g_ruuvi_data.last_update == 0 ||
        pdTICKS_TO_MS(xTaskGetTickCount() - g_ruuvi_data.last_update) > (1000 * 60 * 60))
    {
        ESP_LOGW(TAG, "Ruuvi pressure data unavailable or too old, skipping publish");
        return;
    }

    char body[64];
    snprintf(body, sizeof(body), "{\"value\":\"%.2f\"}", g_ruuvi_data.pressure_mmhg);

    esp_http_client_config_t config = {
        .url = AIO_PRESSURE_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client==nullptr) return;
    esp_http_client_set_header(client, "X-AIO-Key", CONFIG_PWS_ADAFRUIT_IO_KEY);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Pressure publish failed: %s", esp_err_to_name(err));
        return;
    }
    if (status != 200 && status != 201)
    {
        ESP_LOGW(TAG, "Pressure publish HTTP status %d", status);
        return;
    }
    ESP_LOGI(TAG, "Published pressure %.2f mmHg to feed '%s'",
             g_ruuvi_data.pressure_mmhg, CONFIG_PWS_ADAFRUIT_IO_PUBLISH_PRESSURE_FEED);
}

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        s_retry_num++;
        ESP_LOGI(TAG, "Retrying connection %d...", s_retry_num);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
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
            .ssid = CONFIG_PWS_WIFI_SSID,
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

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to '%s'", CONFIG_PWS_WIFI_SSID);
    }
    else
    {
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
        adafruit_fetch();
        adafruit_publish_ruuvi();
        adafruit_publish_pressure();
        aio_chart_fetch();
        vTaskDelay(pdMS_TO_TICKS(FETCH_INTERVAL_MS));
    }
}
