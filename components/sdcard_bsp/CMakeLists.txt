idf_component_register(SRCS "sdcard_bsp.c" "ext4_cache.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES sdmmc esp_driver_sdmmc)
