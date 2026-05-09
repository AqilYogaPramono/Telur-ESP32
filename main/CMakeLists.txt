idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES usb usb_stream xfer_http
                    PRIV_REQUIRES esp_timer lwip)
