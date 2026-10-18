idf_component_register(SRCS "sdcard_bsp.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc)
