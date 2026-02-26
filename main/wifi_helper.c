#include "esp_wifi_types_generic.h"
const wifi_config_t wifi_config = {
    .sta = {
        .ssid = CONFIG_PWS_WIFI_SSID,
        .password = CONFIG_PWS_WIFI_PASSWORD,
    },
};
