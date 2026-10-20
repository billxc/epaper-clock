#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#define CLOCK_WIDTH 800
#define CLOCK_HEIGHT 480
/* All renderers output packed 2bpp, MSB first: 0 black, 1 dark gray,
 * 2 light gray, 3 white. This format does not change in monochrome mode. */
#define CLOCK_FRAME_SIZE (CLOCK_WIDTH * CLOCK_HEIGHT / 4)
#define CLOCK_MONO_FRAME_SIZE (CLOCK_WIDTH * CLOCK_HEIGHT / 8)
/* UTF-8. Eight visible lines, scrolling to keep selected visible; -1 means
 * no selection. Long lines clip at the right margin. Unknown glyphs -> box. */
void screen_render(uint8_t *frame, const char *title,
                   const char *const *lines, size_t count, int selected);
void management_render(uint8_t *frame, const char *password, const char *ip);
void provision_render(uint8_t *frame, const char *service_name, const char *pop, bool initial);
void clock_render(uint8_t *frame, const struct tm *time, bool valid_time,
                  float temperature, float humidity, int battery);
