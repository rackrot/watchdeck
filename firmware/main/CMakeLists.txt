idf_component_register(SRCS "main.c" "display.c" "touch.c" "ui.c" "proto.c" "icons.c" "pairing.c" "link.c" "sdcard.c" "wifi.c"
                            "usb.c" "crash.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_lcd esp_hw_support esp_driver_gpio fatfs sdmmc
                                     esp_wifi esp_netif mdns esp_event nvs_flash
                                     lvgl esp_lvgl_port esp_tinyusb esp_lcd_ek79007 esp_lcd_touch esp_lcd_touch_gt911 esp_driver_i2c
                                     esp_websocket_client cjson esp_app_format mbedtls
                                     espcoredump)
