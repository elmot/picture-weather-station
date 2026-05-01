#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "power_bsp.h"
#include "XPowersLib.h"

static const char *TAG = "axp2101";

static XPowersPMU axp2101;

static I2cMasterBus           *i2cbus_   = nullptr;
static i2c_master_dev_handle_t i2cPMICdev = nullptr;
static uint8_t                 i2cPMICAddress;

static int AXP2101_SLAVE_Read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    int ret;
    uint8_t count = 3;
    do
    {
        ret = (i2cbus_->i2c_read_buff(i2cPMICdev, regAddr, data, len) == ESP_OK) ? 0 : -1;
        if (ret == 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
        count--;
    } while (count);
    return ret;
}

static int AXP2101_SLAVE_Write(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    int ret;
    uint8_t count = 3;
    do
    {
        ret = (i2cbus_->i2c_write_buff(i2cPMICdev, regAddr, data, len) == ESP_OK) ? 0 : -1;
        if (ret == 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
        count--;
    } while (count);
    return ret;
}

void Custom_PmicPortGpioInit() {
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << AXP2101_iqr_PIN) | (1ULL << AXP2101_CHGLED_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
}

void Custom_PmicPortInit(I2cMasterBus *i2cbus,uint8_t dev_addr) {
    if(i2cbus_ == nullptr) {
        i2cbus_ = i2cbus;
    }
    if(i2cPMICdev == nullptr) {
        i2c_master_bus_handle_t BusHandle = i2cbus_->Get_I2cBusHandle();
        i2c_device_config_t     dev_cfg   = {};
        dev_cfg.dev_addr_length           = I2C_ADDR_BIT_LEN_7;
        dev_cfg.scl_speed_hz              = 10000;
        dev_cfg.device_address            = dev_addr;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(BusHandle, &dev_cfg, &i2cPMICdev));
        i2cPMICAddress = dev_addr;
    }
    if (axp2101.begin(i2cPMICAddress, AXP2101_SLAVE_Read, AXP2101_SLAVE_Write)) {
        ESP_LOGI(TAG, "Init PMU SUCCESS!");
    } else {
        ESP_LOGE(TAG, "Init PMU FAILED!");
    }
    Custom_PmicPortGpioInit();
    Custom_PmicRegisterInit();
}

void Custom_PmicRegisterInit(void) {
    axp2101.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_2000MA);

    if(axp2101.getDC1Voltage() != 3300) {
        axp2101.setDC1Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set DCDC1 to output 3V3");
    }
    if(axp2101.getALDO1Voltage() != 3300) {
        axp2101.setALDO1Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set ALDO1 to output 3V3");
    }
    if(axp2101.getALDO2Voltage() != 3300) {
        axp2101.setALDO2Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set ALDO2 to output 3V3");
    }
    if(axp2101.getALDO3Voltage() != 3300) {
        axp2101.setALDO3Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set ALDO3 to output 3V3");
    }
    if(axp2101.getALDO4Voltage() != 3300) {
        axp2101.setALDO4Voltage(3300);
        ESP_LOGW("axp2101_init_log","Set ALDO4 to output 3V3");
    }

    axp2101.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    axp2101.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
    axp2101.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);

    /* ALDO1,2, BLDOs, DC2-4 are unused on this board — keep them off*/
    axp2101.disableALDO1();
    axp2101.disableALDO2();
    axp2101.disableBLDO1();
    axp2101.disableBLDO2();
    axp2101.disableDC2();
    axp2101.disableDC3();
    axp2101.disableDC4();
}

/*-----------------------------------------------------------------------
 * GPIO3 (AXP2101 CHG_LED) → GPIO42 (external charge LED) mirror.
 * The ISR fires on either edge of GPIO3 and copies the level to GPIO42.
 *---------------------------------------------------------------------*/
static void IRAM_ATTR chgled_mirror_isr(void *)
{
    gpio_set_level(POWER_CHGLED_MIRROR_PIN,
                   gpio_get_level(AXP2101_CHGLED_PIN));
}

void power_gpio_init(void)
{
    /* GPIO45: output, drive low while MCU is awake. */
    gpio_config_t out_conf = {};
    out_conf.intr_type    = GPIO_INTR_DISABLE;
    out_conf.mode         = GPIO_MODE_OUTPUT;
    out_conf.pin_bit_mask = (1ULL << POWER_MCU_ACTIVE_PIN)
                          | (1ULL << POWER_CHGLED_MIRROR_PIN);
    out_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    gpio_config(&out_conf);

    /* Releasing any hold left over from a previous deep-sleep cycle. */
    gpio_hold_dis(POWER_MCU_ACTIVE_PIN);
    gpio_set_level(POWER_MCU_ACTIVE_PIN, 0);

    /* GPIO3: input, any-edge interrupt for the mirror. */
    gpio_config_t in_conf = {};
    in_conf.intr_type    = GPIO_INTR_ANYEDGE;
    in_conf.mode         = GPIO_MODE_INPUT;
    in_conf.pin_bit_mask = 1ULL << AXP2101_CHGLED_PIN;
    in_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    in_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&in_conf);

    /* GPIO5: external wake input. Idle low (pull-down), woken on rising
     * edge by esp_deep_sleep_enable_gpio_wakeup() configured at sleep entry. */
    gpio_config_t wake_conf = {};
    wake_conf.intr_type    = GPIO_INTR_DISABLE;
    wake_conf.mode         = GPIO_MODE_INPUT;
    wake_conf.pin_bit_mask = 1ULL << POWER_WAKE_PIN;
    wake_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    wake_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    gpio_config(&wake_conf);

    /* Initial mirror state. */
    gpio_set_level(POWER_CHGLED_MIRROR_PIN,
                   gpio_get_level(AXP2101_CHGLED_PIN));

    /* Install ISR service (idempotent — returns ESP_ERR_INVALID_STATE if
     * already installed). */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
        return;
    }
    gpio_isr_handler_add(AXP2101_CHGLED_PIN, chgled_mirror_isr, nullptr);
}

volatile battery_status_t battery_status{};

/*-----------------------------------------------------------------------
 * One-shot battery / charger state dump.
 *---------------------------------------------------------------------*/
void power_read_battery()
{
    bool charging = axp2101.isCharging();
    uint16_t v_batt = axp2101.getBattVoltage();
    uint16_t v_vbus = axp2101.getVbusVoltage();
    uint16_t v_sys  = axp2101.getSystemVoltage();
    unsigned short pct    = axp2101.getBatteryPercent();
    uint8_t  cs     = axp2101.getChargerStatus();
    const char *cs_str;
    switch (cs) {
        case XPOWERS_AXP2101_CHG_TRI_STATE:  cs_str = "tri-charge";       break;
        case XPOWERS_AXP2101_CHG_PRE_STATE:  cs_str = "pre-charge";       break;
        case XPOWERS_AXP2101_CHG_CC_STATE:   cs_str = "constant current"; break;
        case XPOWERS_AXP2101_CHG_CV_STATE:   cs_str = "constant voltage"; break;
        case XPOWERS_AXP2101_CHG_DONE_STATE: cs_str = "done";             break;
        case XPOWERS_AXP2101_CHG_STOP_STATE: cs_str = "not charging";     break;
        default: cs_str = "unknown"; break;
    }
    ESP_LOGI(TAG, "Battery: %dmV (%d%%), VBUS=%dmV, VSYS=%dmV, charging=%s, status=%s",
             v_batt, pct, v_vbus, v_sys, charging ? "yes" : "no", cs_str);
    battery_status.batteryVoltageMV = v_batt;
    battery_status.batteryPercent = pct;
    battery_status.charging = charging;
}

/*-----------------------------------------------------------------------
 * Log why we just booted — useful when debugging deep-sleep behaviour.
 *---------------------------------------------------------------------*/
void power_log_boot_reason(void)
{
    const esp_reset_reason_t r = esp_reset_reason();
    const uint32_t w = esp_sleep_get_wakeup_causes();
    ESP_LOGI(TAG, "Boot: reset_reason=%d, wakeup_cause=%d", (int)r, (int)w);
}

/*-----------------------------------------------------------------------
 * Last-step preparation before esp_deep_sleep_start():
 *   - shut DC4 off (per board policy)
 *   - drive the MCU-active flag GPIO45 high and lock it for sleep
 *---------------------------------------------------------------------*/
void power_pre_deep_sleep(void)
{
    axp2101.disableDC3();
    axp2101.disableDC4();
    ESP_LOGI(TAG, "Disabled DCDC3, DCDC4 before deep sleep");

    gpio_set_level(POWER_MCU_ACTIVE_PIN, 1);
    gpio_hold_en(POWER_MCU_ACTIVE_PIN);
    gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG, "GPIO%d held high for deep sleep", POWER_MCU_ACTIVE_PIN);

    /* Wake the chip when the external GPIO5 line goes high. */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(POWER_WAKE_PIN, 1));
    ESP_LOGI(TAG, "Wake on GPIO%d HIGH armed", POWER_WAKE_PIN);
}

