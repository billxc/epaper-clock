cmake_minimum_required(VERSION 3.16)
# Release builds identify the exact source even when container Git ownership
# prevents ESP-IDF's automatic repository-version discovery.
if(DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 PROJECT_VER)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epaper-clock)
