#pragma once
#include "driver/gpio.h"
esp_err_t rtc_gpio_hold_dis(gpio_num_t pin);
esp_err_t rtc_gpio_deinit(gpio_num_t pin);
esp_err_t rtc_gpio_pullup_en(gpio_num_t pin);
esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin);
