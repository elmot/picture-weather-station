idf_component_register(SRCS "picture-weather-station.c" aht20_task.c wifi_sta.c ruuvi_task.c weather_fetch.c
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver esp_wifi esp_event nvs_flash bt esp_timer esp_http_client esp-tls
                    EMBED_FILES "${PROJECT_DIR}/pict/wind-fox-a.png")