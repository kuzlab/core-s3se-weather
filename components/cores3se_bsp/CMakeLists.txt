idf_component_register(SRCS "cores3se_bsp.c" "cores3se_display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_i2c esp_driver_gpio
                                esp_driver_spi esp_lcd
                                aw9523b axp2101 bm8563)
