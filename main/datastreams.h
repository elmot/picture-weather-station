#pragma once

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

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

void pushRuuviData(const ruuvi_data_t* data);

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

extern QueueHandle_t g_meteo_queue; /* depth-1 queue, use xQueueOverwrite / xQueuePeek */

/*-----------------------------------------------------------------------
 * SHTC3 indoor sensor
 *---------------------------------------------------------------------*/
void pushShtc3Data(float temperature, float humidity);
/*-----------------------------------------------------------------------
 * Adafruit IO feed
 *---------------------------------------------------------------------*/
typedef struct
{
    float value;
    TickType_t last_update;
} adafruit_data_t;

/*-----------------------------------------------------------------------
 * Adafruit IO chart data
 *---------------------------------------------------------------------*/
#define AIO_CHART_MAX 96

typedef struct
{
    float values[AIO_CHART_MAX];
    int count;
} chart_data_t;

/*-----------------------------------------------------------------------
 * Last fetched local time (formatted by Adafruit IO strftime endpoint).
 * Returns a pointer to a static buffer; empty string if not yet fetched.
 *---------------------------------------------------------------------*/
const char* web_get_last_time(void);

#ifdef __cplusplus
}
#endif
