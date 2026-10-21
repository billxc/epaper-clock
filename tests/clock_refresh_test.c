#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "clock_refresh.h"

int main(void)
{
    // Scheduled cold boots with valid mono cache: partial except periodic full.
    for (int minute = 0; minute < 60; ++minute) {
        const char *reason = clock_full_refresh_reason(true, false, false, false,
                                                       true, minute, true, false);
        if (minute == 0 || minute == 30) assert(reason && !strcmp(reason, "periodic"));
        else assert(!reason);
    }
    assert(clock_full_refresh_reason(true, true, false, false, true, 1, true, false));
    assert(!clock_full_refresh_reason(false, true, false, false, true, 1, true, false));
    assert(clock_full_refresh_reason(false, false, true, false, true, 1, true, false));
    assert(clock_full_refresh_reason(false, false, false, true, true, 1, true, false));
    assert(clock_full_refresh_reason(false, false, false, false, false, 1, true, false));
    assert(clock_full_refresh_reason(true, false, false, false, true, 1, false, false));
    assert(clock_full_refresh_reason(false, false, false, false, true, 1, true, true));
    puts("Clock minute refresh policy tests passed");
    return 0;
}
