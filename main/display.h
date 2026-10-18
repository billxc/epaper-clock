#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#define CLOCK_WIDTH 800
#define CLOCK_HEIGHT 480
#define CLOCK_FRAME_SIZE (CLOCK_WIDTH * CLOCK_HEIGHT / 8)
void clock_render(uint8_t *frame, const struct tm *time, bool valid_time,
                  float temperature, float humidity, int battery);
