idf_component_register(SRCS "cores3se_bsp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_i2c esp_driver_gpio
                                aw9523b axp2101 bm8563)
