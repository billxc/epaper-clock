#pragma once
#include <stdbool.h>

// Keep the minute-boot policy independent of the filesystem and panel drivers.
// NULL means a verified old monochrome frame may be used for a partial update.
static inline const char *clock_full_refresh_reason(bool first, bool manual_start,
    bool synced, bool dirty, bool valid_time, int minute, bool mono, bool mode_changed)
{
    if (!mono) return "gray_mode";
    if (first && manual_start) return "manual_entry";
    if (mode_changed) return "mode_changed";
    if (synced) return "time_synced";
    if (dirty) return "time_changed";
    if (!valid_time) return "invalid_rtc";
    if (minute == 0 || minute == 30) return "periodic";
    return 0;
}
