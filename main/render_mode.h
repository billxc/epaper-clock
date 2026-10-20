#pragma once
#include <stdbool.h>

typedef enum {
    RENDER_MONO = 0,
    RENDER_LINEAR = 1,
    RENDER_INK_MATCH = 2,
} render_mode_t;

// Active mode changes only on the UI task, between complete frames.
render_mode_t render_mode_get(void);
void render_mode_set(render_mode_t mode);
const char *render_mode_name(render_mode_t mode);
bool render_mode_parse(const char *name, render_mode_t *mode);
