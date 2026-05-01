#pragma once

#include "i2c_bsp.h"

#define AXP2101_iqr_PIN             GPIO_NUM_21
#define AXP2101_CHGLED_PIN          GPIO_NUM_3

/* Discrete power-control pins driven by the SoC (not on the PMIC). */
#define POWER_MCU_ACTIVE_PIN        GPIO_NUM_45  /* low while MCU runs, high in deep sleep */
#define POWER_CHGLED_MIRROR_PIN     GPIO_NUM_42  /* mirrors AXP2101_CHGLED_PIN while awake */
#define POWER_WAKE_PIN              GPIO_NUM_5   /* external wake input — high triggers deep-sleep wake */

void Custom_PmicPortInit(I2cMasterBus *i2cbus,uint8_t dev_addr);
void Custom_PmicRegisterInit();
[[noreturn]]void Axp2101_isChargingTask(void *arg);

/* New power-management helpers. */
void power_gpio_init();          /* configure 45/42/3, install GPIO3→42 ISR */
void power_log_battery();        /* one-shot dump of PMIC charge/battery state */
void power_log_boot_reason();    /* log esp_reset_reason + wake cause at boot */
void power_pre_deep_sleep();     /* DC4 off, GPIO45 driven high & held for sleep */
