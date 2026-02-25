#pragma once

#include <math.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------------------
 * Ruuvi tag data
 *---------------------------------------------------------------------*/
typedef struct
{
    float temperature; /* C   */
    float humidity; /* %RH */
    float pressure_mmhg; /* mmHg */
    float battery_voltage; /* V   */
    uint8_t mac_address[6]; /* MAC address */
    TickType_t last_update;
} ruuvi_data_t;

extern volatile ruuvi_data_t g_ruuvi_data;

/*-----------------------------------------------------------------------
 * Internet weather (open-meteo)
 *---------------------------------------------------------------------*/
typedef struct
{
    float temp; /* °C */
    float feels; /* °C apparent */
    float humidity; /* %RH */
    float wind_speed; /* m/s */
    float wind_gusts; /* m/s */
    const char* wind_dir; /* "N","NE",… */
    float pressure; /* hPa */
    int code; /* WMO weather code */
    bool is_day; /* true if daytime */
    TickType_t last_update;
} meteo_data_t;

extern volatile meteo_data_t g_meteo;

/*-----------------------------------------------------------------------
 * AHT20 indoor sensor
 *---------------------------------------------------------------------*/
void pushAht20Data(float temperature, float humidity);
void recordRuuviData(void);
/*-----------------------------------------------------------------------
 * Adafruit IO feed
 *---------------------------------------------------------------------*/
typedef struct
{
    float value;
    TickType_t last_update;
} adafruit_data_t;

extern volatile adafruit_data_t g_adafruit;

/*-----------------------------------------------------------------------
 * Adafruit IO chart data
 *---------------------------------------------------------------------*/
#define AIO_CHART_MAX 96
extern float g_aio_chart[AIO_CHART_MAX];
extern volatile int g_aio_chart_count;

#ifdef __cplusplus
}
#endif
