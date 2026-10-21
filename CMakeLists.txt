cmake_minimum_required(VERSION 3.16)
# Release builds identify the exact source even when container Git ownership
# prevents ESP-IDF's automatic repository-version discovery.
if(DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 PROJECT_VER)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper-clock)

# ffconf.h in IDF 5.5.2 hard-codes exFAT off. Load that exact SDK config
# first, then override it project-locally. PUBLIC is essential: FATFS/FIL and
# FSIZE_t must have the same ABI in the library AND every C/C++ consumer.
idf_component_get_property(epaper_fatfs_target fatfs COMPONENT_LIB)
idf_component_get_property(epaper_fatfs_dir fatfs COMPONENT_DIR)
get_filename_component(epaper_sdk_ffconf
    "${epaper_fatfs_dir}/src/ffconf.h" ABSOLUTE)
target_compile_definitions(${epaper_fatfs_target} PUBLIC
    EPAPER_SDK_FFCONF_PATH="${epaper_sdk_ffconf}")
target_compile_options(${epaper_fatfs_target} PUBLIC
    "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/components/sdcard_bsp/fatfs_exfat_config.h\">")
