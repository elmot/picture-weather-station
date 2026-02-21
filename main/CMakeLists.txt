idf_component_register(SRCS "picture-weather-station.c" aht20_task.c
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver
                    EMBED_FILES "${PROJECT_DIR}/pict/wind-fox-a.png")