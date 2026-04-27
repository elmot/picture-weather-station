//
// Created by elmot on 22/02/2026.
//

#ifndef PICTURE_WEATHER_STATION_HW_H
#define PICTURE_WEATHER_STATION_HW_H

#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_H_RES        800
#define LCD_V_RES        480

extern esp_lcd_panel_handle_t s_panel;

void hw_init(void);

void epaper_sleep(void);

#ifdef __cplusplus
}
#endif

#endif //PICTURE_WEATHER_STATION_HW_H
