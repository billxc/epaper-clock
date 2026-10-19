#pragma once
#include <stdbool.h>
void usb_time_init(bool connected);
// Nonblocking; true only when a TIME command changed the hardware RTC.
bool usb_time_poll(void);
