idf_component_register(SRCS "main.c" "judge.c" "ui.c" "app_state.c"
                            "wifi.c" "time_sync.c" "weather_openmeteo.c"
                       INCLUDE_DIRS "."
                       REQUIRES cores3se_bsp nvs_flash esp_timer fonts
                                esp_wifi esp_netif esp_event
                                esp_http_client esp-tls mbedtls json lwip)
