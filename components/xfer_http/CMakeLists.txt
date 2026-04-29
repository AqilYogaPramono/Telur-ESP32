idf_component_register(SRCS "app_httpd.c" "app_wifi.c"
                    INCLUDE_DIRS "." "include"
                    PRIV_REQUIRES esp_wifi esp_timer nvs_flash lwip esp_http_server esp_http_client json mbedtls)
