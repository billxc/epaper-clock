#include "render_mode.h"
#include <stdatomic.h>
#include <string.h>

static atomic_int active;
static const char *const names[] = {"mono", "linear", "ink_match"};
render_mode_t render_mode_get(void) { return (render_mode_t)atomic_load(&active); }
void render_mode_set(render_mode_t mode) { atomic_store(&active, mode); }
const char *render_mode_name(render_mode_t mode) { return names[mode]; }
bool render_mode_parse(const char *name, render_mode_t *mode)
{
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(name, names[i])) { *mode = (render_mode_t)i; return true; }
    return false;
}
