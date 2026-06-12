#pragma once

/*-----------------------------------------------------------------------
 * WMO weather interpretation codes (WW)
 * https://www.nodc.noaa.gov/archive/arc0021/0002199/1.1/data/0-data/HTML/WMO-CODE/WMO4677.HTM
 *---------------------------------------------------------------------*/
typedef enum
{
    WMO_CLEAR_SKY = 0,
    WMO_MAINLY_CLEAR = 1,
    WMO_PARTLY_CLOUDY = 2,
    WMO_OVERCAST = 3,
    WMO_FOG = 45,
    WMO_RIME_FOG = 48,
    WMO_DRIZZLE_LIGHT = 51,
    WMO_DRIZZLE_MODERATE = 53,
    WMO_DRIZZLE_DENSE = 55,
    WMO_FREEZING_DRIZZLE_LIGHT = 56,
    WMO_FREEZING_DRIZZLE_DENSE = 57,
    WMO_RAIN_SLIGHT = 61,
    WMO_RAIN_MODERATE = 63,
    WMO_RAIN_HEAVY = 65,
    WMO_FREEZING_RAIN_LIGHT = 66,
    WMO_FREEZING_RAIN_HEAVY = 67,
    WMO_SNOW_SLIGHT = 71,
    WMO_SNOW_MODERATE = 73,
    WMO_SNOW_HEAVY = 75,
    WMO_SNOW_GRAINS = 77,
    WMO_RAIN_SHOWERS_SLIGHT = 80,
    WMO_RAIN_SHOWERS_MODERATE = 81,
    WMO_RAIN_SHOWERS_VIOLENT = 82,
    WMO_SNOW_SHOWERS_SLIGHT = 85,
    WMO_SNOW_SHOWERS_HEAVY = 86,
    WMO_THUNDERSTORM = 95,
    WMO_THUNDERSTORM_HAIL_SLIGHT = 96,
    WMO_THUNDERSTORM_HAIL_HEAVY = 99,
} wmo_weather_code_t;


inline FoxConditionEnum fox_condition(const int wmo_weather_code_t,
    const bool is_day,
    const float wind_speed_ms,
    const float wind_gusts_ms)
{
    const bool is_windy = wind_speed_ms >= 10.0f || wind_gusts_ms >= 15.0f;
    switch (wmo_weather_code_t)
    {
    case WMO_CLEAR_SKY:
    case WMO_MAINLY_CLEAR:
    case WMO_PARTLY_CLOUDY:
        if (is_windy) return FoxConditionEnum::Windy;
        return is_day ? FoxConditionEnum::Sunny : FoxConditionEnum::Good;
    case WMO_OVERCAST:
        return is_windy ? FoxConditionEnum::Windy : FoxConditionEnum::Cloudy;
    case WMO_DRIZZLE_LIGHT:
    case WMO_DRIZZLE_MODERATE:
    case WMO_DRIZZLE_DENSE:
    case WMO_FREEZING_DRIZZLE_LIGHT:
    case WMO_FREEZING_DRIZZLE_DENSE:
        return is_windy ? FoxConditionEnum::Windy : FoxConditionEnum::Drizzly;
    case WMO_FOG:
    case WMO_RIME_FOG:
        return FoxConditionEnum::Foggy;
    case WMO_RAIN_SLIGHT:
    case WMO_RAIN_MODERATE:
    case WMO_RAIN_HEAVY:
    case WMO_FREEZING_RAIN_LIGHT:
    case WMO_FREEZING_RAIN_HEAVY:
    case WMO_RAIN_SHOWERS_SLIGHT:
    case WMO_RAIN_SHOWERS_MODERATE:
    case WMO_RAIN_SHOWERS_VIOLENT:
    case WMO_THUNDERSTORM:
        return is_windy ? FoxConditionEnum::WindyRainy : FoxConditionEnum::Rainy;
    case WMO_SNOW_SLIGHT:
    case WMO_SNOW_MODERATE:
    case WMO_SNOW_HEAVY:
    case WMO_SNOW_GRAINS:
    case WMO_SNOW_SHOWERS_SLIGHT:
    case WMO_SNOW_SHOWERS_HEAVY:
    case WMO_THUNDERSTORM_HAIL_SLIGHT:
    case WMO_THUNDERSTORM_HAIL_HEAVY:
        return FoxConditionEnum::Snowy;
    }
    return FoxConditionEnum::Good;
}
