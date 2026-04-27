#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "shtc3.h"
#include "datastreams.h"

static const char *TAG = "sensor-ws";

extern i2c_master_bus_handle_t s_i2c_bus;

static i2c_master_dev_handle_t s_shtc3 = NULL;

void shtc3_init(void)
{
    s_shtc3 = shtc3_device_create(s_i2c_bus, SHTC3_I2C_ADDR, CONFIG_SHTC3_I2C_CLK_SPEED_HZ);
    if (s_shtc3 == NULL) {
        ESP_LOGE(TAG, "SHTC3 init failed");
        return;
    }
    ESP_LOGI(TAG, "SHTC3 initialised");
}

bool shtc3_read_once(float *t, float *h)
{
    if (s_shtc3 == NULL) return false;
    if (shtc3_get_th(s_shtc3, SHTC3_REG_T_CSE_NM, t, h) != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 read failed");
        return false;
    }
    ESP_LOGI(TAG, "SHTC3: %.1f C, %.1f %%RH", *t, *h);
    return true;
}
