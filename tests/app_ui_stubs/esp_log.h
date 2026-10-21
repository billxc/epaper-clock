#pragma once
#include <stdio.h>
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
