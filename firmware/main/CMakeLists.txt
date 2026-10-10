idf_component_register(SRCS "main.c" "display.c" "sdcard.c" "wifi.c" "usb.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_lcd esp_hw_support esp_driver_gpio fatfs sdmmc
                                     esp_wifi esp_netif mdns esp_event nvs_flash
                                     lvgl esp_lvgl_port esp_tinyusb esp_lcd_ek79007)
