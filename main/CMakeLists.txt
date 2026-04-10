         idf_component_register(
                 SRCS
                    picture-weather-station.cpp
                    hw_support.c
                    shtc3_task.c
                    ruuvi_task.c
                    web_task.cpp
                    chart.cpp
                    wifi_helper.c
                 INCLUDE_DIRS "."
                 REQUIRES esp_lcd driver esp_wifi esp_event nvs_flash bt esp_timer esp_http_client esp-tls
         )

         slint_target_sources(${COMPONENT_LIB} ../ui/weather-station.slint)
