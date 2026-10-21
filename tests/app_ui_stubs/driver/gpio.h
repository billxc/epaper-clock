#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
enum { GPIO_NUM_4 = 4, GPIO_NUM_5 = 5, GPIO_NUM_6 = 6,
       GPIO_MODE_INPUT = 1, GPIO_PULLUP_ENABLE = 1,
       GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0 };
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_config(const gpio_config_t *config);
