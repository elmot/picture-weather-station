#pragma once

#include <math.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------
 * Ruuvi tag data
 *---------------------------------------------------------------------*/
typedef struct {
    float temperature;      /* C   */
    float humidity;         /* %RH */
    float pressure_mmhg;   /* mmHg */
    float battery_voltage;  /* V   */
    uint8_t mac_address[6]; /* MAC address */
} ruuvi_data_t;

extern ruuvi_data_t g_ruuvi_data;
extern volatile TickType_t ruuvi_last_update;

/*-----------------------------------------------------------------------
 * Internet weather (open-meteo)
 *---------------------------------------------------------------------*/
typedef struct {
    float temp;           /* °C */
    float feels;          /* °C apparent */
    float humidity;       /* %RH */
    float wind_speed;     /* m/s */
    float wind_gusts;     /* m/s */
    const char* wind_dir; /* "N","NE",… */
    float pressure;       /* hPa */
    int code;             /* WMO weather code */
    char* icon_png;       /* pointer to embedded PNG data */
    TickType_t last_update;
} meteo_data_t;

extern volatile meteo_data_t g_meteo;

/*-----------------------------------------------------------------------
 * AHT20 indoor sensor
 *---------------------------------------------------------------------*/
typedef struct {
    float temperature;    /* °C */
    float humidity;       /* %RH */
} aht20_data_t;

extern volatile aht20_data_t g_aht20;

#ifdef __cplusplus
}
#endif
