idf_component_register(
    SRCS "main.c" "rtc.c" "display.c" "network.c" "admin.c"
    INCLUDE_DIRS "."
    REQUIRES i2c_bsp axpPower shtc3_bsp epaper_port sdcard_bsp
             esp_driver_gpio esp_driver_usb_serial_jtag esp_timer nvs_flash
             esp_wifi esp_netif esp_event lwip esp_http_server mbedtls json
             esp_hw_support
    EMBED_TXTFILES "admin.html")
