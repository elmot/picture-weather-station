         file(GLOB PNG_FILES "${PROJECT_DIR}/pict/svg_png/wmo_icon_*.png")

         idf_component_register(
                 SRCS
                    picture-weather-station.cpp
                    hw_support.c
                    aht20_task.c
                    ruuvi_task.c
                    weather_fetch.c
                 INCLUDE_DIRS "."
                 REQUIRES esp_lcd driver esp_wifi esp_event nvs_flash bt esp_timer esp_http_client esp-tls
                 EMBED_FILES
                    ${PNG_FILES}
                    ${PROJECT_DIR}/pict/wifi-wait.png
         )

         slint_target_sources(${COMPONENT_LIB} weather-station.slint)
