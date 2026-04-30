#include "driver/i2c_master.h"
#include "esp_log.h"
#include "hw_support.h"
#include "i2c_bsp.h"
#include "power_bsp.h"
#include "epaper.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

static const char* TAG = "hw";

/*-----------------------------------------------------------------------
 * I2C bus
 *---------------------------------------------------------------------*/
#define I2C_SDA          GPIO_NUM_47
#define I2C_SCL          GPIO_NUM_48

i2c_master_bus_handle_t s_i2c_bus;
I2cMasterBus *s_i2c_bus_obj;

epd_handle_t s_epd = nullptr;

/*-----------------------------------------------------------------------
 * Initialise the e-paper display
 *---------------------------------------------------------------------*/
static void epaper_init()
{
    epd_config_t epd_cfg = EPD_CONFIG_73_6COLOR();

    esp_err_t ret = epd_init(&epd_cfg, &s_epd);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init e-paper: %s", esp_err_to_name(ret));
        return;
    }

    epd_panel_info_t info;
    epd_get_info(s_epd, &info);
    ESP_LOGI(TAG, "E-paper: %dx%d, buf=%lu bytes", info.width, info.height,
             (unsigned long)info.buffer_size);
}

/*------------------------------------------------------------------------
 * Initialise I2C master bus + PMIC
 *---------------------------------------------------------------------*/
static void i2c_init()
{
    s_i2c_bus_obj = new I2cMasterBus(I2C_SCL, I2C_SDA, I2C_NUM_0);
    s_i2c_bus = s_i2c_bus_obj->Get_I2cBusHandle();
    ESP_LOGI(TAG, "I2C initialised (SDA=%d, SCL=%d)", I2C_SDA, I2C_SCL);

    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan complete");

    Custom_PmicPortInit(s_i2c_bus_obj, 0x34);
}


void hw_init()
{
    power_gpio_init();
    i2c_init();
    epaper_init();
}

void epaper_sleep()
{
  epd_sleep(s_epd);
}

epd_handle_t epaper_handle()
{
    return s_epd;
}
/*-----------------------------------------------------------------------
 * Override the e-paper component's __weak idle hook. The default does
 * vTaskDelay(5ms) inside the busy-pin polling loop. The 6-color panel
 * stays busy for ~30 s per refresh — light-sleep instead, so the SoC
 * draws ~µA between polls. Wakeup source is the timer; the panel's
 * BUSY pin transition is detected on the next poll.
 *---------------------------------------------------------------------*/

extern "C" void epd_idle(uint32_t /*ms*/)
{
    esp_sleep_enable_timer_wakeup(300ULL * 1000ULL);  /* 300 ms */
    gpio_hold_en(POWER_MCU_ACTIVE_PIN);
    esp_light_sleep_start();
    gpio_hold_dis(POWER_MCU_ACTIVE_PIN);
}

