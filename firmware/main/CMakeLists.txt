# S3a seam, replaced in S3b by the cert stored at pairing: the daemon cert the link pins is
# compiled in from main/daemon_cert.pem.local, a gitignored copy of the daemon's identity.pem.
# Without that file the build embeds an empty cert and the link refuses to connect. CMake tracks
# the file once it exists, but run `idf.py reconfigure` after creating it.
set(pinned_cert "${CMAKE_CURRENT_BINARY_DIR}/daemon_cert.pem")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/daemon_cert.pem.local")
        configure_file("${CMAKE_CURRENT_LIST_DIR}/daemon_cert.pem.local" "${pinned_cert}" COPYONLY)
    else()
        file(CONFIGURE OUTPUT "${pinned_cert}" CONTENT "")
    endif()
endif()

idf_component_register(SRCS "main.c" "display.c" "touch.c" "ui.c" "proto.c" "link.c" "sdcard.c" "wifi.c" "usb.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "${pinned_cert}"
                       PRIV_REQUIRES esp_lcd esp_hw_support esp_driver_gpio fatfs sdmmc
                                     esp_wifi esp_netif mdns esp_event nvs_flash
                                     lvgl esp_lvgl_port esp_tinyusb esp_lcd_ek79007 esp_lcd_touch esp_lcd_touch_gt911 esp_driver_i2c
                                     esp_websocket_client cjson esp_app_format mbedtls)
