#pragma once
#include "../../app_ui_stubs/freertos/task.h"
#define portMAX_DELAY UINT32_MAX
void vTaskDelay(TickType_t ticks);
