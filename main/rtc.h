#pragma once
#include <stdbool.h>
#include <time.h>
// Read before rtc_init clears the alarm: distinguishes scheduled vs manual start.
bool rtc_alarm_pending(void);
void rtc_init(void);
bool rtc_read(struct tm *time);
void rtc_set(time_t epoch);
void rtc_arm_next_minute(void);
