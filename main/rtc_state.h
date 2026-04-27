#pragma once

#include "sensor_history.h"

struct shtc3_reading_t
{
    float temperature;
    float humidity;
};

void rtc_history_save(const SensorHistory<shtc3_reading_t, 96> &src);
void rtc_history_restore(SensorHistory<shtc3_reading_t, 96> &dst);
