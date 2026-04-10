#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "shtc3.h"
#include "datastreams.h"

static const char *TAG = "sensor-ws";

#define SHTC3_INTERVAL   (1 * 60 * 1000)      /* 1 min */

extern i2c_master_bus_handle_t s_i2c_bus;



_Noreturn void sensor_task(void *arg)
{
    ESP_UNUSED(arg);
    i2c_master_dev_handle_t s_shtc3 = shtc3_device_create(s_i2c_bus, SHTC3_I2C_ADDR, CONFIG_SHTC3_I2C_CLK_SPEED_HZ);
    ESP_ERROR_CHECK(s_shtc3 ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "SHTC3 initialised");
    for (;;) {
        float t, h;
        if (shtc3_get_th(s_shtc3, SHTC3_REG_T_CSE_NM, &t, &h) == ESP_OK) {
            pushShtc3Data(t, h);
            ESP_LOGI(TAG, "SHTC3: %.1f °C, %.1f %%RH", t, h);
        }
        vTaskDelay(pdMS_TO_TICKS(SHTC3_INTERVAL));
    }
}
