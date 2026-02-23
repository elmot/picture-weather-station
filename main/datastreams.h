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
    TickType_t last_update;
} ruuvi_data_t;

extern volatile ruuvi_data_t g_ruuvi_data;

/*-----------------------------------------------------------------------
 * WMO weather interpretation codes (WW)
 * https://www.nodc.noaa.gov/archive/arc0021/0002199/1.1/data/0-data/HTML/WMO-CODE/WMO4677.HTM
 *---------------------------------------------------------------------*/
typedef enum {
    WMO_CLEAR_SKY           =  0,
    WMO_MAINLY_CLEAR        =  1,
    WMO_PARTLY_CLOUDY       =  2,
    WMO_OVERCAST            =  3,
    WMO_FOG                 = 45,
    WMO_RIME_FOG            = 48,
    WMO_DRIZZLE_LIGHT       = 51,
    WMO_DRIZZLE_MODERATE    = 53,
    WMO_DRIZZLE_DENSE       = 55,
    WMO_FREEZING_DRIZZLE_LIGHT = 56,
    WMO_FREEZING_DRIZZLE_DENSE = 57,
    WMO_RAIN_SLIGHT         = 61,
    WMO_RAIN_MODERATE       = 63,
    WMO_RAIN_HEAVY          = 65,
    WMO_FREEZING_RAIN_LIGHT = 66,
    WMO_FREEZING_RAIN_HEAVY = 67,
    WMO_SNOW_SLIGHT         = 71,
    WMO_SNOW_MODERATE       = 73,
    WMO_SNOW_HEAVY          = 75,
    WMO_SNOW_GRAINS         = 77,
    WMO_RAIN_SHOWERS_SLIGHT = 80,
    WMO_RAIN_SHOWERS_MODERATE = 81,
    WMO_RAIN_SHOWERS_VIOLENT = 82,
    WMO_SNOW_SHOWERS_SLIGHT = 85,
    WMO_SNOW_SHOWERS_HEAVY  = 86,
    WMO_THUNDERSTORM        = 95,
    WMO_THUNDERSTORM_HAIL_SLIGHT = 96,
    WMO_THUNDERSTORM_HAIL_HEAVY  = 99,
} wmo_weather_code_t;

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
    bool is_day;          /* true if daytime */
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

/*-----------------------------------------------------------------------
 * Adafruit IO feed
 *---------------------------------------------------------------------*/
typedef struct {
    float value;
    TickType_t last_update;
} adafruit_data_t;

extern volatile adafruit_data_t g_adafruit;

#ifdef __cplusplus
}
#endif
