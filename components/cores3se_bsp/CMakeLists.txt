idf_component_register(SRCS "cores3se_bsp.c" "cores3se_display.c" "cores3se_touch.c"
                       INCLUDE_DIRS "include"
                       # esp_lcd_touch is public: cores3se_bsp.h exposes
                       # esp_lcd_touch_handle_t, so dependents need its headers.
                       REQUIRES driver esp_driver_i2c esp_driver_gpio
                                esp_driver_spi esp_lcd
                                espressif__esp_lcd_touch
                                aw9523b axp2101 bm8563)
