idf_component_register(SRCS "app_main.c" "i2c_bsp.cpp" "XPowersLibInterface.cpp" "power_bsp.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer driver esp_driver_i2c)
