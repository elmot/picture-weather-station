#include <cstring>
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "datastreams.h"
#include "sensor_history.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_heap_caps.h"

constexpr uint32_t WIFI_CONNECTED_BIT = BIT0;
constexpr uint32_t WIFI_FAIL_BIT = BIT1;

constexpr size_t HTTP_BUF_SIZE = 8192;

static_assert(sizeof(CONFIG_PWS_WIFI_SSID) > 1, "WiFi SSID can not be empty. "
              "Define CONFIG_PWS_WIFI_SSID via `idf.py menuconfig` or in `skdconfig.secrets` file");

static_assert(sizeof(CONFIG_PWS_WIFI_PASSWORD) > 1, "WiFi password can not be empty. "
              "Define CONFIG_PWS_WIFI_PASSWORD via `idf.py menuconfig` or in `skdconfig.secrets` file");

constexpr const char* TAG = "wifi";
constexpr const char* WEB_TAG = "WEB";
constexpr const char* AIO_TAG = "AdafruitIO";
QueueHandle_t g_meteo_queue;
extern SensorHistory<ruuvi_data_t, 1> g_ruuvi_history;
extern SensorHistory<adafruit_data_t, 1> g_adafruit_history;
extern SensorHistory<adafruit_data_t, 1> g_adafruit_humidity_history;
extern SensorHistory<adafruit_data_t, 1> g_adafruit_temperature_history;
extern SensorHistory<chart_data_t, 1> g_chart_history;
extern SensorHistory<chart_data_t, 1> g_pressure_chart_history;

#define WEATHER_URL "https://api.open-meteo.com/v1/forecast?latitude=" CONFIG_PWS_LAT \
"&longitude=" CONFIG_PWS_LON \
"&models=metno_seamless&current=temperature_2m,relative_humidity_2m,apparent_temperature,snowfall,showers,rain,is_day,precipitation,weather_code,cloud_cover,pressure_msl,surface_pressure,wind_gusts_10m,wind_direction_10m,wind_speed_10m&wind_speed_unit=ms"

/*-----------------------------------------------------------------------
 * HTTP event handler — accumulates response body into user_data buffer
 *---------------------------------------------------------------------*/
struct http_buf_t
{
    char* buf;
    size_t len;
    size_t cap;
};

template <size_t CAPACITY>
class PsramBuf
{
    char* p_ = nullptr;

public:
    char* get()
    {
        if (!p_)
        {
            p_ = static_cast<char*>(heap_caps_malloc(CAPACITY, MALLOC_CAP_SPIRAM));
            if (!p_)
            {
                ESP_LOGE(WEB_TAG, "PSRAM alloc failed");
                abort();
            }
        }
        return p_;
    }

    static constexpr size_t cap() { return CAPACITY; }

    char const* sprintf(const char* format, ...)
    {
        char* p = get();
        va_list args;
        va_start(args, format);
        vsnprintf(p, CAPACITY, format, args);
        va_end(args);
        return p;
    }
};


static PsramBuf<HTTP_BUF_SIZE> s_http_buf;

extern "C" {
static esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    auto* resp = static_cast<http_buf_t*>(evt->user_data);
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
}

/*-----------------------------------------------------------------------
 * Common http helper
 *---------------------------------------------------------------------*/
static bool web_or_adafruit_io_access(const char* url,
                                      bool adafruit_io_key,
                                      const char* body,
                                      const char* label, http_buf_t* response,
                                      http_event_handle_cb http_cb)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = body == nullptr ? HTTP_METHOD_GET : HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    config.event_handler = http_cb;
    config.user_data = response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr)
    {
        ESP_LOGE(WEB_TAG, "HTTP client init error");
        return false;
    }

    if (adafruit_io_key)
    {
        esp_http_client_set_header(client, "X-AIO-Key", CONFIG_PWS_ADAFRUIT_IO_KEY);
    }

    if (body != nullptr)
    {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, static_cast<int>(strlen(body)));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(adafruit_io_key ? AIO_TAG : WEB_TAG, "%s failed: %s", label, esp_err_to_name(err));
        return false;
    }
    if (status != 200 && status != 201)
    {
        ESP_LOGW(adafruit_io_key ? AIO_TAG : WEB_TAG, "%s HTTP status %d", label, status);
        return false;
    }
    return true;
}


/*-----------------------------------------------------------------------
 * Fetch JSON from URL and parse with cJSON
 *---------------------------------------------------------------------*/
constexpr const char* wind_names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

static int cJsonGetInt(const cJSON* current, const char* key, int def = 0)
{
    const auto item = cJSON_GetObjectItem(current, key);

    return cJSON_IsNumber(item) ? item->valueint : def;
}

static void weather_fetch_and_parse()
{
    http_buf_t resp = {.buf = s_http_buf.get(), .len = 0, .cap = s_http_buf.cap()};
    if (!web_or_adafruit_io_access(WEATHER_URL, false, nullptr, "Weather fetch", &resp, http_event_handler))
    {
        return;
    }

    cJSON* root = cJSON_Parse(resp.buf);
    const cJSON* current = cJSON_GetObjectItem(root, "current");

    if (!current)
    {
        ESP_LOGE(WEB_TAG, "JSON parse error");
        cJSON_Delete(root);
        return;
    }

    const int wind_dir_deg = cJsonGetInt(current, "wind_direction_10m", 0);
    const unsigned int wind_sector = ((wind_dir_deg + 22) % 360 / 45) % 8;

    meteo_data_t meteo = {
        .temp = static_cast<float>(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "temperature_2m"))),
        .feels = static_cast<float>(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "apparent_temperature"))),
        .humidity = static_cast<float>(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "relative_humidity_2m"))),
        .wind_speed = static_cast<float>(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_speed_10m"))),
        .wind_gusts = static_cast<float>(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "wind_gusts_10m"))),
        .wind_dir = wind_names[wind_sector],
        .pressure = static_cast<float>(cJSON_GetNumberValue(cJSON_GetObjectItem(current, "pressure_msl"))),
        .code = cJsonGetInt(current, "weather_code", -1),
        .is_day = cJsonGetInt(current, "is_day") != 0,
        .last_update = xTaskGetTickCount(),
    };

    cJSON_Delete(root);

    xQueueOverwrite(g_meteo_queue, &meteo);

    ESP_LOGI(WEB_TAG, "Weather Code: %d", meteo.code);
    ESP_LOGI(WEB_TAG, "Temperature: %.1f C", meteo.temp);
    ESP_LOGI(WEB_TAG, "Humidity: %.0f%%", meteo.humidity);
    ESP_LOGI(WEB_TAG, "Wind: %s  %.1f m/s (gusts: %.1f m/s)",
             meteo.wind_dir, meteo.wind_speed, meteo.wind_gusts);
}

/*-----------------------------------------------------------------------
 * Fetch latest value from Adafruit IO feed
 *---------------------------------------------------------------------*/
#define AIO_LAST_URL_FMT "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/feeds/%s/data/last"

static void adafruit_fetch_feed(const char* feed_key,
                                SensorHistory<adafruit_data_t, 1>& dest)
{
    char url[256];
    snprintf(url, sizeof(url), AIO_LAST_URL_FMT, feed_key);

    http_buf_t resp = {.buf = s_http_buf.get(), .len = 0, .cap = s_http_buf.cap()};
    if (!web_or_adafruit_io_access(url, true, nullptr, "Adafruit fetch",
                                   &resp, http_event_handler))
        return;
    cJSON* root = cJSON_Parse(resp.buf);
    if (!root)
    {
        ESP_LOGE(AIO_TAG, "Adafruit IO JSON parse error");
        return;
    }

    if (const char* value_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "value")))
    {
        float val = strtof(value_str, nullptr);
        dest.push({val, xTaskGetTickCount()});
        ESP_LOGI(AIO_TAG, "Adafruit IO: %s = %.2f", feed_key, val);
    }

    cJSON_Delete(root);
}

/*-----------------------------------------------------------------------
 * Publish Ruuvi sensor data as JSON to Adafruit IO feed
 *---------------------------------------------------------------------*/
#define AIO_PUBLISH_URL "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/feeds/" CONFIG_PWS_ADAFRUIT_IO_PUBLISH_JSON_FEED "/data"

static void adafruit_publish_ruuvi()
{
    auto ruuvi = g_ruuvi_history.last();
    if (ruuvi.last_update == 0) return;

    char const* body = s_http_buf.sprintf(R"({"value":"{\"temperature\":%.1f,\"pressure\":%.0f,\"humidity\":%.2f}"})",
                                          ruuvi.temperature, ruuvi.pressure_mmhg, ruuvi.humidity);

    web_or_adafruit_io_access(AIO_PUBLISH_URL, true, body, "Ruuvi publish to " CONFIG_PWS_ADAFRUIT_IO_PUBLISH_JSON_FEED,
                              nullptr,
                              nullptr);
}

/*-----------------------------------------------------------------------
 * Publish Ruuvi pressure to a dedicated Adafruit IO feed
 *---------------------------------------------------------------------*/
#define AIO_PRESSURE_URL "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/feeds/" CONFIG_PWS_ADAFRUIT_IO_PRESSURE_FEED_KEY "/data"


/*-----------------------------------------------------------------------
 * Fetch chart data from Adafruit IO
 * Response: { "columns":["date","avg"], "data":[["...", "val"], ...] }
 * Empty buckets come back with a null value — stored as NaN so the
 * chart renderer leaves a gap instead of drawing through.
 *---------------------------------------------------------------------*/
#define AIO_CHART_URL_FMT "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/feeds/%s/data/chart?hours=48&resolution=30&field=avg"

static void adafruit_io_chart_fetch_feed(const char* feed_key,
                                         SensorHistory<chart_data_t, 1>& dest)
{
    char url[256];
    snprintf(url, sizeof(url), AIO_CHART_URL_FMT, feed_key);

    http_buf_t resp = {.buf = s_http_buf.get(), .len = 0, .cap = s_http_buf.cap()};
    if (!web_or_adafruit_io_access(url, true, nullptr,
                                   "AIO chart fetch", &resp, http_event_handler))
        return;
    ESP_LOGI(AIO_TAG, "AIO chart '%s': received %d bytes",
             feed_key, static_cast<int>(resp.len));

    cJSON* root = cJSON_Parse(resp.buf);
    if (!root)
    {
        ESP_LOGE(AIO_TAG, "AIO chart: JSON parse error");
        return;
    }

    cJSON* data_arr = cJSON_GetObjectItem(root, "data");
    if (!data_arr || !cJSON_IsArray(data_arr))
    {
        ESP_LOGE(AIO_TAG, "AIO chart: missing 'data' array");
        cJSON_Delete(root);
        return;
    }

    /* Each element is [timestamp_str, value] — already chronological */
    chart_data_t chart{};
    int valid = 0;
    cJSON const* row;
    cJSON_ArrayForEach(row, data_arr)
    {
        if (chart.count >= AIO_CHART_MAX) break;
        if (!cJSON_IsArray(row)) continue;
        cJSON* val = cJSON_GetArrayItem(row, 1);
        float v = NAN;
        if (val)
        {
            if (const char* s = cJSON_GetStringValue(val))
            {
                v = strtof(s, nullptr);
            }
            else if (cJSON_IsNumber(val))
            {
                v = static_cast<float>(cJSON_GetNumberValue(val));
            }
        }
        chart.values[chart.count++] = v;
        if (std::isfinite(v)) valid++;
    }

    cJSON_Delete(root);

    dest.push(chart);
    ESP_LOGI(AIO_TAG, "Chart '%s': %d slots (%d with values)",
             feed_key, chart.count, valid);
}

/*-----------------------------------------------------------------------
 * Fetch current local time from Adafruit IO time integration.
 * Response is plain text formatted by strftime.
 *---------------------------------------------------------------------*/
#define AIO_TIME_URL "https://io.adafruit.com/api/v2/" CONFIG_PWS_ADAFRUIT_IO_USER \
    "/integrations/time/strftime?tz=Europe/Helsinki&strftime=%25H:%25M%20%25d-%25b-%25Y"

static char s_last_time[64] = {};

static void adafruit_time_fetch()
{
    http_buf_t resp = {.buf = s_http_buf.get(), .len = 0, .cap = s_http_buf.cap()};
    if (!web_or_adafruit_io_access(AIO_TIME_URL, true, nullptr,
                                   "AIO time fetch", &resp, http_event_handler))
        return;
    while (resp.len > 0)
    {
        const char c = resp.buf[resp.len - 1];
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t') break;
        resp.len--;
    }
    resp.buf[resp.len] = '\0';
    snprintf(s_last_time, sizeof(s_last_time), "%s", resp.buf);
    ESP_LOGI(AIO_TAG, "AIO time: '%s'", s_last_time);
}

extern "C" const char* web_get_last_time(void)
{
    return s_last_time;
}

static void adafruit_publish_pressure()
{
    if constexpr (sizeof(CONFIG_PWS_ADAFRUIT_IO_PRESSURE_FEED_KEY) <= 1)
    {
        ESP_LOGW(AIO_TAG, "Pressure feed key not configured, skipping publish");
        // ReSharper disable once CppDFAUnreachableCode
        return;
    }
    auto ruuvi = g_ruuvi_history.last();
    if (ruuvi.last_update == 0 ||
        pdTICKS_TO_MS(xTaskGetTickCount() - ruuvi.last_update) > (1000 * 60 * 60))
    {
        ESP_LOGW(AIO_TAG, "Ruuvi pressure data unavailable or too old, skipping publish");
        return;
    }

    char const* body = s_http_buf.sprintf(R"({"value":"%.2f"})", ruuvi.pressure_mmhg);

    if (web_or_adafruit_io_access(AIO_PRESSURE_URL, true, body, "Pressure publish", nullptr,
                                  nullptr))
    {
        ESP_LOGI(AIO_TAG, "Published pressure %.2f mmHg to feed '%s'",
                 ruuvi.pressure_mmhg, CONFIG_PWS_ADAFRUIT_IO_PRESSURE_FEED_KEY);
    }
}

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;

extern "C" {
extern const wifi_config_t wifi_config;

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
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
}

/*-----------------------------------------------------------------------
 * Start Wi-Fi (non-blocking — returns once esp_wifi_start() is issued).
 *---------------------------------------------------------------------*/
extern "C" void wifi_start(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    static esp_event_handler_instance_t instance_any_id;
    static esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, &instance_got_ip));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, const_cast<wifi_config_t*>(&wifi_config)));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", CONFIG_PWS_WIFI_SSID);
}

/*-----------------------------------------------------------------------
 * Block until Wi-Fi has an IP, or timeout.
 *---------------------------------------------------------------------*/
extern "C" bool wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_wifi_event_group == nullptr) return false;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to '%s'", CONFIG_PWS_WIFI_SSID);
        return true;
    }
    ESP_LOGW(TAG, "Could not connect to '%s'", CONFIG_PWS_WIFI_SSID);
    return false;
}

/*-----------------------------------------------------------------------
 * One-shot read of remote sources. Caller must have already confirmed
 * a Wi-Fi connection via wifi_wait_connected().
 *---------------------------------------------------------------------*/
extern "C" void wifi_fetch_remote_data(void)
{
    weather_fetch_and_parse();
    adafruit_fetch_feed(CONFIG_PWS_ADAFRUIT_IO_CO2_FEED_KEY, g_adafruit_history);
    if constexpr (sizeof(CONFIG_PWS_ADAFRUIT_IO_HUMIDITY_FEED_KEY) > 1)
    {
        adafruit_fetch_feed(CONFIG_PWS_ADAFRUIT_IO_HUMIDITY_FEED_KEY,
                            g_adafruit_humidity_history);
    }
    if constexpr (sizeof(CONFIG_PWS_ADAFRUIT_IO_TEMPERATURE_FEED_KEY) > 1)
    {
        adafruit_fetch_feed(CONFIG_PWS_ADAFRUIT_IO_TEMPERATURE_FEED_KEY,
                            g_adafruit_temperature_history);
    }
    adafruit_io_chart_fetch_feed(CONFIG_PWS_ADAFRUIT_IO_CO2_FEED_KEY, g_chart_history);
    if constexpr (sizeof(CONFIG_PWS_ADAFRUIT_IO_PRESSURE_FEED_KEY) > 1)
    {
        adafruit_io_chart_fetch_feed(CONFIG_PWS_ADAFRUIT_IO_PRESSURE_FEED_KEY,
                                     g_pressure_chart_history);
    }
    adafruit_time_fetch();
}

/*-----------------------------------------------------------------------
 * Publish Ruuvi tag readings to Adafruit IO. Caller should have a
 * fresh Ruuvi sample (otherwise the publish helpers are no-ops).
 *---------------------------------------------------------------------*/
extern "C" void wifi_publish_ruuvi(void)
{
    adafruit_publish_ruuvi();
    adafruit_publish_pressure();
}
