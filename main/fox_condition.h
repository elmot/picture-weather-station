#pragma once
inline FoxConditionEnum fox_condition(const int wmo_weather_code_t)
{
    switch (g_meteo.code)
    {
    case WMO_CLEAR_SKY:
    case WMO_MAINLY_CLEAR:
    case WMO_PARTLY_CLOUDY:
    case WMO_OVERCAST:
    case WMO_FOG:
    case WMO_RIME_FOG:
        return FoxConditionEnum::Sun;
    case WMO_DRIZZLE_LIGHT:
    case WMO_DRIZZLE_MODERATE:
    case WMO_DRIZZLE_DENSE:
    case WMO_FREEZING_DRIZZLE_LIGHT:
    case WMO_FREEZING_DRIZZLE_DENSE:
    case WMO_RAIN_SLIGHT:
    case WMO_RAIN_MODERATE:
    case WMO_RAIN_HEAVY:
    case WMO_FREEZING_RAIN_LIGHT:
    case WMO_FREEZING_RAIN_HEAVY:
    case WMO_RAIN_SHOWERS_SLIGHT:
    case WMO_RAIN_SHOWERS_MODERATE:
    case WMO_RAIN_SHOWERS_VIOLENT:
    case WMO_THUNDERSTORM:
        return FoxConditionEnum::Rain;
    case WMO_SNOW_SLIGHT:
    case WMO_SNOW_MODERATE:
    case WMO_SNOW_HEAVY:
    case WMO_SNOW_GRAINS:
    case WMO_SNOW_SHOWERS_SLIGHT:
    case WMO_SNOW_SHOWERS_HEAVY:
    case WMO_THUNDERSTORM_HAIL_SLIGHT:
    case WMO_THUNDERSTORM_HAIL_HEAVY:
        return FoxConditionEnum::Snow;
    }
    return FoxConditionEnum::Sun;
}
