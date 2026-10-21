/* Host UI integration: real app_ui/menu/render-mode, no hardware or SD IO.
 * cc -std=c11 -Wall -Wextra -Werror -Itests/app_ui_stubs -Imain \
 *   -Icomponents/epaper_port tests/app_ui_test.c main/app_ui.c \
 *   main/menu_model.c main/render_mode.c -o /tmp/app-ui-test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_ui.h"
#include "display.h"
#include "menu_model.h"
#include "render_settings.h"
#include "epaper_port.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static menu_input_t queue[32];
static unsigned queued;
static int64_t time_us;
static int task_token, queue_token;
static void *allocations[3];
static unsigned allocated;
static unsigned transfers, inits, gray_inits, sleeps, invalidates;
static unsigned fail_transfer;
static bool fail_init;
static char kinds[3];
static const uint8_t *pointers[3];
static uint8_t frames[3][CLOCK_FRAME_SIZE];
static uint8_t successful_menu[CLOCK_FRAME_SIZE], clock_image[CLOCK_FRAME_SIZE];

void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(size == CLOCK_FRAME_SIZE && caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(allocated < 3);
    void *p = malloc(size);
    assert(p);
    allocations[allocated++] = p;
    return p;
}
const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock error"; }
int64_t esp_timer_get_time(void) { return time_us; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &task_token; }
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelayUntil(TickType_t *wake, TickType_t ticks) { *wake += ticks; }
BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    return pdPASS; // Scanner must not run; tests inject logical queue events.
}
QueueHandle_t xQueueCreate(unsigned length, size_t item_size)
{
    assert(length == 32 && item_size == sizeof(menu_input_t));
    return &queue_token;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)wait;
    assert(q == &queue_token && queued < 32);
    queue[queued++] = *(const menu_input_t *)item;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)wait;
    assert(q == &queue_token);
    if (!queued) return 0;
    *(menu_input_t *)item = queue[0];
    --queued;
    memmove(queue, queue + 1, queued * sizeof(*queue));
    return pdTRUE;
}
void xQueueReset(QueueHandle_t q) { assert(q == &queue_token); queued = 0; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return 1; }
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
void cache_invalidate(void) { ++invalidates; }
esp_err_t render_settings_fallback(void) { render_mode_set(RENDER_MONO); return ESP_OK; }

/* Deterministic whole-frame renderer distinguishes menu, confirmation, message,
 * selection and render mode. Real glyph/layout coverage lives in display_test. */
void screen_render(uint8_t *frame, const char *title, const char *const *lines,
                   size_t count, int selected)
{
    (void)lines;
    memset(frame, 0xff, CLOCK_FRAME_SIZE);
    size_t length = strlen(title);
    memcpy(frame, title, length);
    frame[length] = (uint8_t)count;
    frame[length + 1] = (uint8_t)selected;
    frame[length + 2] = (uint8_t)render_mode_get();
    frame[CLOCK_FRAME_SIZE - 1] = (uint8_t)selected;
}
esp_err_t EPD_Init(void) { ++inits; return fail_init ? ESP_FAIL : ESP_OK; }
esp_err_t EPD_Init_4GRAY(void) { ++gray_inits; return fail_init ? ESP_FAIL : ESP_OK; }
void EPD_Sleep(void) { ++sleeps; }
static esp_err_t transfer(char kind, const uint8_t *image)
{
    assert(transfers < 3 && image);
    kinds[transfers] = kind;
    pointers[transfers] = image;
    memcpy(frames[transfers], image, CLOCK_FRAME_SIZE);
    ++transfers;
    return fail_transfer == transfers ? ESP_FAIL : ESP_OK;
}
esp_err_t EPD_Display_Base(const uint8_t *image) { return transfer('B', image); }
esp_err_t EPD_Display_4Gray(const uint8_t *image) { return transfer('G', image); }
esp_err_t EPD_Display_Partial(const uint8_t *image, UWORD x, UWORD y, UWORD w, UWORD h)
{
    assert(x == 0 && y == 0 && w == EPD_WIDTH && h == EPD_HEIGHT);
    return transfer('P', image);
}
static void reset_calls(void)
{
    transfers = inits = gray_inits = sleeps = invalidates = 0;
    fail_transfer = 0;
    fail_init = false;
}
static app_ui_event_t click(menu_input_t input)
{
    assert(xQueueSend(&queue_token, &input, 0));
    return app_ui_poll();
}
static void expect_full_menu(void)
{
    assert(transfers == 1 && kinds[0] == 'B');
    assert(inits == 1 && gray_inits == 0 && sleeps == 1 && invalidates == 1);
    assert(app_ui_display_ok() && app_ui_menu_open() && !app_ui_clock_visible());
    memcpy(successful_menu, frames[0], CLOCK_FRAME_SIZE);
}
static void expect_partial_menu(void)
{
    assert(transfers == 2 && kinds[0] == 'P' && kinds[1] == 'P');
    assert(inits == 1 && gray_inits == 0 && sleeps == 1 && invalidates == 1);
    assert(pointers[0] != pointers[1]);
    assert(memcmp(frames[0], successful_menu, CLOCK_FRAME_SIZE) == 0);
    assert(memcmp(frames[0], frames[1], CLOCK_FRAME_SIZE) != 0);
    assert(app_ui_display_ok() && !app_ui_clock_visible());
    memcpy(successful_menu, frames[1], CLOCK_FRAME_SIZE);
}
static void move(menu_input_t input)
{
    reset_calls();
    click(input);
    expect_partial_menu();
}
int main(void)
{
    app_ui_init();
    app_ui_init(); // Idempotent allocation: three distinct lifetime buffers.
    assert(allocated == 3);
    memset(clock_image, 0x33, sizeof(clock_image));
    app_ui_present(clock_image, true);
    assert(app_ui_clock_visible());

    reset_calls();
    click(MENU_INPUT_LONG); // Physical entry cannot use clock as previous.
    expect_full_menu();
    move(MENU_INPUT_DOWN);
    move(MENU_INPUT_UP);
    reset_calls();
    app_ui_refresh_menu();
    assert(transfers == 0 && inits == 0 && invalidates == 0);
    menu_input_t up = MENU_INPUT_UP, down = MENU_INPUT_DOWN;
    xQueueSend(&queue_token, &up, 0);
    xQueueSend(&queue_token, &down, 0);
    app_ui_poll(); // Coalesced events leave the same visible menu.
    assert(transfers == 0);

    /* Hidden background rendering must not replace the visible menu baseline,
     * even when a message overwrites the shared canvas. */
    memset(clock_image, 0xcc, sizeof(clock_image));
    app_ui_present_clock(NULL, clock_image, false);
    const char *lines[] = {"hidden background"};
    app_ui_message("message", lines, 1);
    assert(transfers == 0 && !app_ui_clock_visible());
    move(MENU_INPUT_UP); // Initialize entry.
    move(MENU_INPUT_SELECT); // Confirmation page.
    move(MENU_INPUT_DOWN); // Yes.
    move(MENU_INPUT_UP); // No.
    move(MENU_INPUT_SELECT); // Cancel, return to menu.
    move(MENU_INPUT_SELECT);
    move(MENU_INPUT_DOWN);
    reset_calls();
    app_ui_event_t event = click(MENU_INPUT_SELECT);
    assert(event.action == APP_INITIALIZE && !app_ui_menu_open());
    assert(transfers == 0 && inits == 0 && invalidates == 0);
    assert(!app_ui_clock_visible()); // Glass still holds menu, not background.
    app_ui_message("action target", lines, 1);
    assert(transfers == 1 && kinds[0] == 'B'); // Only supervisor target draws.
    reset_calls();
    app_ui_open_menu();
    expect_full_menu();

    /* Failure at init, first partial, or second partial invalidates trust. */
    for (unsigned failure = 0; failure <= 2; ++failure) {
        reset_calls();
        fail_init = failure == 0;
        fail_transfer = failure;
        click(MENU_INPUT_DOWN);
        assert(!app_ui_display_ok());
        assert(transfers == failure);
        reset_calls();
        app_ui_refresh_menu(); // Same new canvas still must recover via full.
        expect_full_menu();
        move(MENU_INPUT_UP);
    }
    reset_calls();
    fail_transfer = 1;
    app_ui_open_menu(); // A failed full menu cannot be reused either.
    assert(!app_ui_display_ok());
    reset_calls();
    app_ui_refresh_menu();
    expect_full_menu();

    for (render_mode_t mode = RENDER_LINEAR; mode <= RENDER_INK_MATCH; ++mode) {
        render_mode_set(mode);
        reset_calls();
        app_ui_refresh_menu();
        assert(transfers == 1 && kinds[0] == 'G');
        assert(gray_inits == 1 && inits == 0 && invalidates == 0);
        reset_calls();
        click(MENU_INPUT_DOWN);
        assert(transfers == 1 && kinds[0] == 'G');
        assert(gray_inits == 1 && inits == 0 && invalidates == 0);
    }
    render_mode_set(RENDER_MONO);
    reset_calls();
    app_ui_refresh_menu();
    expect_full_menu();
    move(MENU_INPUT_DOWN);

    /* Successful fallback after gray failure is not a mono menu baseline. */
    render_mode_set(RENDER_LINEAR);
    reset_calls();
    fail_transfer = 1;
    app_ui_refresh_menu();
    assert(transfers == 2 && kinds[0] == 'G' && kinds[1] == 'B');
    assert(app_ui_display_ok() && render_mode_get() == RENDER_MONO);
    reset_calls();
    app_ui_refresh_menu();
    expect_full_menu();

    /* Mode change observed by hidden clock presentation revokes menu trust. */
    render_mode_set(RENDER_LINEAR);
    reset_calls();
    app_ui_present(clock_image, true);
    assert(transfers == 0);
    render_mode_set(RENDER_MONO);
    app_ui_refresh_menu();
    expect_full_menu();
    reset_calls();
    app_ui_set_mode(APP_LAN);
    app_ui_refresh_menu();
    expect_full_menu();

    reset_calls();
    app_ui_open_menu(); // Reopen at current APP_LAN, then select same mode.
    expect_full_menu();
    reset_calls();
    event = click(MENU_INPUT_SELECT);
    assert(event.action == APP_NONE && !app_ui_menu_open());
    assert(transfers == 1 && kinds[0] == 'B'); // No-action close still restores.
    reset_calls();
    click(MENU_INPUT_LONG);
    expect_full_menu();
    reset_calls();
    time_us += 30000000;
    app_ui_poll(); // Idle exit restores background full, never menu partial.
    assert(!app_ui_menu_open() && transfers == 1 && kinds[0] == 'B');
    reset_calls();
    app_ui_message("outside menu", lines, 1);
    assert(transfers == 1 && kinds[0] == 'B');
    reset_calls();
    click(MENU_INPUT_LONG);
    expect_full_menu();
    move(MENU_INPUT_UP); // From current APP_LAN entry to APP_CLOCK.
    reset_calls();
    event = click(MENU_INPUT_SELECT);
    assert(event.action == APP_CLOCK && !app_ui_menu_open());
    assert(transfers == 0 && !app_ui_clock_visible());
    /* Even a caller requesting partial via the convenience API has no usable
     * background baseline after an action exit; it must draw its target full. */
    app_ui_present(clock_image, false);
    assert(transfers == 1 && kinds[0] == 'B' && app_ui_clock_visible());
    reset_calls();
    app_ui_open_menu();
    expect_full_menu(); // Target clock drawing also cannot preserve menu trust.

    for (unsigned i = 0; i < allocated; ++i) free(allocations[i]);
    puts("app_ui menu refresh tests passed");
    return 0;
}
