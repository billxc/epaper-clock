idf_component_register(
    SRCS "main.c" "rtc.c" "display.c"
    INCLUDE_DIRS "."
    REQUIRES i2c_bsp axpPower shtc3_bsp epaper_port sdcard_bsp
             esp_driver_gpio esp_driver_usb_serial_jtag esp_timer)
