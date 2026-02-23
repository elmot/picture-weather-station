#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "datastreams.h"

static const char *TAG = "sensor-ws";

/*-----------------------------------------------------------------------
 * AHT20 temperature & humidity sensor
 *---------------------------------------------------------------------*/
#define AHT20_ADDR       0x38
#define AHT20_CMD_INIT   0xBE
#define AHT20_CMD_MEAS   0xAC
#define AHT20_INTERVAL   (5 * 1000)      /* 5 sec in ms *///todo set to 15 min
#define AHT20_HISTORY    96              /* 24 h worth of readings */
#define I2C_TIMEOUT      100
typedef struct {
    float temperature;   /* °C  */
    float humidity;      /* %RH */
} sensor_reading_t;

volatile aht20_data_t g_aht20 = { .temperature = NAN, .humidity = NAN };

static i2c_master_dev_handle_t s_aht20;
static sensor_reading_t s_readings[AHT20_HISTORY];
static int              s_reading_count;        /* total stored so far */
static int              s_reading_head;         /* next write index   */

#define I2C_CLK_HZ       (400 * 1000)
extern i2c_master_bus_handle_t s_i2c_bus;
/*-----------------------------------------------------------------------
 * Initialise AHT20 sensor
 *---------------------------------------------------------------------*/
static void aht20_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AHT20_ADDR,
        .scl_speed_hz    = I2C_CLK_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_aht20));

    /* Calibration command */
    uint8_t cmd[] = { AHT20_CMD_INIT, 0x08, 0x00 };
    ESP_ERROR_CHECK(i2c_master_transmit(s_aht20, cmd, sizeof(cmd), I2C_TIMEOUT));
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "AHT20 initialised");
}

/*-----------------------------------------------------------------------
 * Read AHT20 → temperature (°C) and humidity (%RH)
 * Returns ESP_OK on success.
 *---------------------------------------------------------------------*/
static esp_err_t aht20_read(float *temp, float *hum)
{
    /* Trigger measurement */
    uint8_t cmd[] = { AHT20_CMD_MEAS, 0x33, 0x00 };
    esp_err_t err = i2c_master_transmit(s_aht20, cmd, sizeof(cmd), I2C_TIMEOUT);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(80));

    /* Read 7 bytes: status, hum[19:12], hum[11:4], hum[3:0]|temp[19:16],
       temp[15:8], temp[7:0], crc */
    uint8_t buf[7];
    err = i2c_master_receive(s_aht20, buf, sizeof(buf), I2C_TIMEOUT);
    if (err != ESP_OK) return err;

    if (buf[0] & 0x80) {
        ESP_LOGW(TAG, "AHT20 busy");
        return ESP_ERR_NOT_FINISHED;
    }

    uint32_t raw_hum  = ((uint32_t)buf[1] << 12)
                      | ((uint32_t)buf[2] << 4)
                      | ((uint32_t)buf[3] >> 4);
    uint32_t raw_temp = (((uint32_t)buf[3] & 0x0F) << 16)
                      | ((uint32_t)buf[4] << 8)
                      | ((uint32_t)buf[5]);

    *hum  = (float)(raw_hum * 100.0  / (1 << 20) );
    *temp = (float)(raw_temp * 200.0 / (1 << 20) ) - 50.0f;
    
    return ESP_OK;
}

/*-----------------------------------------------------------------------
 * Sensor task — reads AHT20 every 15 min, stores to ring buffer
 *---------------------------------------------------------------------*/
_Noreturn void sensor_task(void *arg)
{
    ESP_UNUSED(arg);
    aht20_init();

    for (;;) {
        float t, h;
        if (aht20_read(&t, &h) == ESP_OK) {
            g_aht20.temperature = t;
            g_aht20.humidity = h;
            s_readings[s_reading_head].temperature = t;
            s_readings[s_reading_head].humidity    = h;
            s_reading_head = (s_reading_head + 1) % AHT20_HISTORY;
            if (s_reading_count < AHT20_HISTORY) s_reading_count++;
            ESP_LOGI(TAG, "AHT20: %.1f °C, %.1f %%RH  [%d stored]",
                     t, h, s_reading_count);
        }
        vTaskDelay(pdMS_TO_TICKS(AHT20_INTERVAL));
    }
}
