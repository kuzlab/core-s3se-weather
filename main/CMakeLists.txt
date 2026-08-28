idf_component_register(SRCS "main.c" "judge.c"
                       INCLUDE_DIRS "."
                       REQUIRES cores3se_bsp nvs_flash esp_timer)
