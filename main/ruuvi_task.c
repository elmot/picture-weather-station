#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "datastreams.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

static const char* TAG = "ruuvi";

#define RUUVI_COMPANY_ID  0x0499
#define RUUVI_FORMAT      5
/* Pressure conversion: 1 hPa = 0.750062 mmHg */
#define HPA_TO_MMHG       0.750062f

#if CONFIG_PWS_RUUVI_SIGNATURE == 0
#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wcpp"
#warning "Ruuvi tag signature is not defined(0), Ruuvi data is never shown"
#warning "define CONFIG_PWS_RUUVI_SIGNATURE via 'idf.py menuconfig' or in 'skdconfig.secrets' file"
#pragma GCC diagnostic pop
#endif

volatile ruuvi_data_t g_ruuvi_data;

/*-----------------------------------------------------------------------
 * Parse Ruuvi Data Format 5 (RAWv2) — 24-byte payload
 * Struct (big-endian): B h H H h h h H B H 6s
 *   format(1) temp(2) hum(2) press(2) ax(2) ay(2) az(2)
 *   power(2) movement(1) seq(2) mac(6)
 *---------------------------------------------------------------------*/
static bool parse_ruuvi_df5(const uint8_t* data, size_t len, ruuvi_data_t* out)
{
    if (len != 24) return false;
    if (data[0] != RUUVI_FORMAT) return false;

    /* Temperature: int16 big-endian * 0.005 C */
    int16_t temp_raw = (int16_t)((data[1] << 8) | data[2]);
    if (temp_raw == -32768) return false;
    out->temperature = temp_raw * 0.005f;

    /* Humidity: uint16 big-endian * 0.0025 % */
    uint16_t hum_raw = (uint16_t)((data[3] << 8) | data[4]);
    if (hum_raw == 65535) return false;
    out->humidity = hum_raw * 0.0025f;

    /* Pressure: uint16 big-endian, value = raw + 50000 Pa → hPa → mmHg */
    uint16_t press_raw = (uint16_t)((data[5] << 8) | data[6]);
    if (press_raw == 0) return false;
    float pressure_hpa = (press_raw + 50000) / 100.0f;
    out->pressure_mmhg = pressure_hpa * HPA_TO_MMHG;

    /* Battery voltage: bits 5-15 of power field (offset 13-14) */
    uint16_t power_raw = (uint16_t)((data[13] << 8) | data[14]);
    uint16_t battery_bits = power_raw >> 5;
    if (battery_bits == 0x7FF) return false;
    out->battery_voltage = (battery_bits + 1600) / 1000.0f;

    /* MAC address: bytes 18-23 */
    memcpy(out->mac_acopare 2 ddress, &data[18], 6);
    out->last_update = xTaskGetTickCount();
    return true;
}

/*-----------------------------------------------------------------------
 * NimBLE GAP scan callback
 *---------------------------------------------------------------------*/
static int ble_gap_event_cb(struct ble_gap_event* event, void* arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    struct ble_gap_disc_desc* desc = &event->disc;

    /* Filter by BLE local name matching configured 4-hex-digit Ruuvi tag name */
    static struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0)
    {
        return 0;
    }

    if (fields.mfg_data == NULL || fields.mfg_data_len < 4) return 0;

    /* First 2 bytes of mfg_data are company ID (little-endian) */
    uint16_t company_id = fields.mfg_data[0] | (fields.mfg_data[1] << 8);
    if (company_id != RUUVI_COMPANY_ID) return 0;

    /* Remaining bytes are the Ruuvi payload */
    const uint8_t* payload = fields.mfg_data + 2;
    size_t payload_len = fields.mfg_data_len - 2;

    ruuvi_data_t parsed;
    if (parse_ruuvi_df5(payload, payload_len, &parsed))
    {
        g_ruuvi_data = parsed;
        ESP_LOGI(TAG, "Ruuvi: %.1f C, %.1f %%RH, %.1f mmHg, %.2f V, MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 parsed.temperature, parsed.humidity,
                 parsed.pressure_mmhg, parsed.battery_voltage,
                 parsed.mac_address[0], parsed.mac_address[1], parsed.mac_address[2],
                 parsed.mac_address[3], parsed.mac_address[4], parsed.mac_address[5]);
    }

    return 0;
}

/*-----------------------------------------------------------------------
 * Start active BLE scan (called when NimBLE host syncs)
 * Active scan is needed to receive scan responses containing the name.
 *---------------------------------------------------------------------*/
static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced, starting active scan");

    struct ble_gap_disc_params scan_params = {
        .passive = 0, /* active scan to get scan responses with name */
        .itvl = 0, /* use defaults */
        .window = 0,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                          &scan_params, ble_gap_event_cb, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
    }
}

/*-----------------------------------------------------------------------
 * NimBLE host task (runs in its own FreeRTOS task)
 *---------------------------------------------------------------------*/
static void nimble_host_task(void* param)
{
    nimble_port_run(); /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/*-----------------------------------------------------------------------
 * Public init
 *---------------------------------------------------------------------*/
void ruuvi_task_init(void)
{
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;

    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "Ruuvi task initialised");
}
