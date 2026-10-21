idf_component_register(SRCS "sdcard_bsp.c" "fat_frame_cache.c" INCLUDE_DIRS "."
                       REQUIRES fatfs
                       PRIV_REQUIRES sdmmc esp_driver_sdmmc esp_timer esp_hw_support)
