#pragma once
#include <stdbool.h>
#include <time.h>
void rtc_init(void);
bool rtc_read(struct tm *time);
void rtc_set(time_t epoch);
void rtc_arm_next_minute(void);
