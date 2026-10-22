#include "app_ui.h"
#include "display.h"
#include "menu_model.h"
#include "render_mode.h"
#include "render_settings.h"
#include "poweroff.h"
#include "esp_log.h"
#include "epaper_port.h"
#include "sdcard_bsp.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

/* Only the initializing (supervisor/UI) task may touch model/frame/display.
 * The scanner owns only debounce state and sends logical events. No PWR GPIO. */
static const gpio_num_t pins[MENU_KEY_COUNT] = {GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6};
static TaskHandle_t owner;
static QueueHandle_t inputs;
static menu_keys_t keys;
static menu_model_t model;
static uint8_t *background, *canvas;
/* Menu glass baseline, never the hidden clock/background or render scratch.
 * Owned for the UI lifetime; only a successful mono menu transfer commits it. */
static uint8_t *menu_previous;
static bool menu_previous_valid;
static bool have_background, panel_background, display_ok;
static uint32_t menu_last_activity;
#define MENU_IDLE_MS 30000U

bool app_ui_owner(void)
{
    return owner != NULL && xTaskGetCurrentTaskHandle() == owner;
}

static unsigned pressed_keys(void)
{
    unsigned pressed = 0;
    for (unsigned i = 0; i < MENU_KEY_COUNT; ++i)
        if (gpio_get_level(pins[i]) == 0) pressed |= MENU_PRESSED(i);
    return pressed;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void scan_keys(void *unused)
{
    (void)unused;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        unsigned events = menu_keys_sample(&keys, pressed_keys(), now_ms());
        /* One queue entry per logical event. No drawing, model mutation, log
         * of secrets, or waiting for the UI task (which may be refreshing EPD).
         * Queue capacity covers many physical clicks during a full refresh;
         * if saturated, new clicks are dropped, never replayed as a long press. */
        const menu_input_t order[] = {MENU_INPUT_UP, MENU_INPUT_DOWN,
                                      MENU_INPUT_LONG, MENU_INPUT_SELECT};
        for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
            if (events & order[i]) (void)xQueueSend(inputs, &order[i], 0);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
}

static void present_panel(const uint8_t *image, const uint8_t *previous, bool full)
{
    bool gray = render_mode_get() != RENDER_MONO;
    /* Any attempted panel write (including a failed one) consumes menu trust.
     * draw_menu alone may establish it again after a successful mono update. */
    menu_previous_valid = false;
    // Invalidate BEFORE touching glass, including menus/messages and failed
    // refreshes. A reset between panel transfer and cache commit must not
    // leave the previous frame marked as matching the panel. Gray never uses SD.
    if (!gray) {
        cache_invalidate(); // Fresh SD session; also forget the RAM baseline.
    }
    esp_err_t err = gray ? EPD_Init_4GRAY() : EPD_Init();
    if (err == ESP_OK) {
        if (gray) err = EPD_Display_4Gray(image);
        else if (!full && previous) {
            err = EPD_Display_Partial(previous, 0, 0, EPD_WIDTH, EPD_HEIGHT);
            if (err == ESP_OK) err = EPD_Display_Partial(image, 0, 0, EPD_WIDTH, EPD_HEIGHT);
        } else err = EPD_Display_Base(image);
    }
    EPD_Sleep();
    if (err != ESP_OK && gray) {
        ESP_LOGW("display", "Gray refresh failed, restoring monochrome: %s", esp_err_to_name(err));
        esp_err_t saved = render_settings_fallback();
        if (saved != ESP_OK) ESP_LOGW("display", "Unable to persist fallback: %s", esp_err_to_name(saved));
        // A failed gray waveform has no usable differential baseline. The
        // active mode is now mono, so invalidate any old mono cache as well.
        cache_invalidate();
        err = EPD_Init();
        if (err == ESP_OK) err = EPD_Display_Base(image);
        EPD_Sleep();
    }
    display_ok = err == ESP_OK;
    if (!display_ok) ESP_LOGE("display", "Panel refresh failed: %s", esp_err_to_name(err));
}

static void draw_frame(const uint8_t *image)
{
    present_panel(image, NULL, true);
}

bool app_ui_display_ok(void) { return display_ok; }

bool app_ui_clock_visible(void)
{
    return app_ui_owner() && !model.open && panel_background && display_ok;
}

static void draw_menu(void)
{
    bool mono = render_mode_get() == RENDER_MONO;
    if (!mono) menu_previous_valid = false;
    if (model.confirming) {
        const char *lines[] = {"取消，返回选单", "确认，进入初始化／配网"};
        screen_render(canvas, "是否进入初始化／配网？", lines, 2, model.confirm_yes ? 1 : 0);
    } else {
        const char *lines[MENU_ITEM_COUNT];
        for (unsigned i = 0; i < MENU_ITEM_COUNT; ++i) lines[i] = menu_model_label(i);
        screen_render(canvas, "模式选单", lines, MENU_ITEM_COUNT, (int)model.selected);
    }
    if (menu_previous_valid && memcmp(menu_previous, canvas, CLOCK_FRAME_SIZE) == 0)
        return;
    present_panel(canvas, menu_previous_valid ? menu_previous : NULL,
                  !menu_previous_valid);
    /* Gray fallback may succeed in mono, but it is not a trusted mono menu
     * render. Require a fresh mono full frame on the next menu redraw. */
    if (display_ok && mono && render_mode_get() == RENDER_MONO) {
        memcpy(menu_previous, canvas, CLOCK_FRAME_SIZE);
        menu_previous_valid = true;
    }
    panel_background = false;
}

void app_ui_init(void)
{
    if (owner) return;
    owner = xTaskGetCurrentTaskHandle();
    background = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    canvas = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    menu_previous = heap_caps_malloc(CLOCK_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(background && canvas && menu_previous ? ESP_OK : ESP_ERR_NO_MEM);
    memset(background, 0xff, CLOCK_FRAME_SIZE);
    menu_model_init(&model, APP_CLOCK);
    inputs = xQueueCreate(32, sizeof(menu_input_t));
    ESP_ERROR_CHECK(inputs ? ESP_OK : ESP_ERR_NO_MEM);
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4) | (1ULL << GPIO_NUM_5) | (1ULL << GPIO_NUM_6),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    /* Held-at-boot keys must first be released, including the up key that
     * main independently samples for forced provisioning. */
    menu_keys_init(&keys, pressed_keys(), now_ms());
    BaseType_t created = xTaskCreate(scan_keys, "ui_keys", 3072, NULL,
                                    tskIDLE_PRIORITY + 2, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

bool app_ui_confirm_wake_menu(void)
{
    if (!app_ui_owner()) return false;
    uint32_t started = now_ms();
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        // Check release before the deadline: a short wake never becomes LONG.
        if (!(pressed_keys() & MENU_PRESSED(MENU_KEY_CENTER))) return false;
        if ((uint32_t)(now_ms() - started) >= MENU_LONG_PRESS_MS) return true;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
    // The scanner still ignores this held-at-boot key until release, so the
    // confirming hold/release cannot be queued again or select a menu item.
}

void app_ui_set_mode(app_action_t mode)
{
    if (!app_ui_owner()) return;
    /* Background completion must not redraw or move an already-open menu. */
    if (model.mode != mode) menu_previous_valid = false;
    menu_model_set_mode(&model, mode);
}

bool app_ui_menu_open(void)
{
    return app_ui_owner() && model.open;
}

void app_ui_open_menu(void)
{
    if (!app_ui_owner()) return;
    menu_previous_valid = false; // Explicit entry starts with a full menu frame.
    menu_model_open(&model);
    menu_last_activity = now_ms();
    draw_menu();
}

void app_ui_refresh_menu(void)
{
    if (app_ui_owner() && model.open) draw_menu();
}

void app_ui_discard_input(void)
{
    if (app_ui_owner()) {
        // OTA suppresses normal app_poll/UI input, but a PWR request may be
        // recorded here and deferred until management safely yields ownership.
        poweroff_sample();
        xQueueReset(inputs);
    }
}

app_ui_event_t app_ui_poll(void)
{
    app_ui_event_t result = {0};
    if (!app_ui_owner()) return result;
    bool was_open = model.open;
    bool dirty = false;
    menu_input_t input;
    while (xQueueReceive(inputs, &input, 0) == pdTRUE) {
        unsigned selected = model.selected;
        bool confirming = model.confirming, yes = model.confirm_yes, open = model.open;
        app_ui_event_t event = menu_model_input(&model, input);
        result.activity |= event.activity;
        if (event.activity) menu_last_activity = now_ms();
        result.select |= event.select;
        result.action = event.action;
        dirty |= selected != model.selected || confirming != model.confirming ||
                 yes != model.confirm_yes || open != model.open;
        /* Let the supervisor consume a task/mode selection or an outside-menu
         * click before interpreting any later queued physical interactions. */
        if (event.action != APP_NONE || event.select || (open && !model.open)) break;
    }
    if (model.open && (uint32_t)(now_ms() - menu_last_activity) >= MENU_IDLE_MS) {
        model.open = model.confirming = model.confirm_yes = false;
        dirty = true;
    }
    if (model.open && dirty) draw_menu();
    else if (!model.open && was_open) {
        menu_previous_valid = false;
        panel_background = false;
        /* A real action draws its target page in the supervisor. Do not flash
         * the old background in between, or claim the menu glass is a clock. */
        if (result.action == APP_NONE) {
            draw_frame(background);  /* No-action close/idle exit stays full. */
            panel_background = display_ok;
        }
    }
    result.menu_open = model.open;
    return result;
}

void app_ui_present_clock(const uint8_t *previous, const uint8_t *image, bool full)
{
    if (!app_ui_owner() || !image) return;
    if (render_mode_get() != RENDER_MONO) menu_previous_valid = false;
    if (!model.open) {
        /* Both monochrome partial activations share one powered session.
         * Gray frames use the vendor's complete four-level update instead. */
        present_panel(image, previous, full);
        panel_background = display_ok;
    }
    /* Copy after transfers so previous may safely refer to our saved frame. */
    memmove(background, image, CLOCK_FRAME_SIZE);
    have_background = true;
}

void app_ui_present(const uint8_t *image, bool full)
{
    const uint8_t *previous = have_background && panel_background ? background : NULL;
    app_ui_present_clock(previous, image, full);
}

bool app_ui_poweroff(void)
{
    if (!app_ui_owner()) return false;
    model.open = model.confirming = model.confirm_yes = false;
    app_ui_discard_input();
    menu_previous_valid = have_background = panel_background = false;
    // Explicit shutdown changes the retained glass even in gray mode. Revoke
    // any formal mono TF baseline first, using existing fail-closed/unknown-file
    // handling. Never store these animation frames as a clock baseline.
    cache_invalidate();
    const char *lines[] = {"正在关机，请稍候", "画面清空后关闭电源"};
    screen_render(canvas, "正在关机", lines, 2, -1);
    // Unlike app_ui_message this MUST draw even if the menu was open.
    draw_frame(canvas);
    if (!display_ok) return false;
    memcpy(menu_previous, canvas, CLOCK_FRAME_SIZE);
    for (unsigned step = 1; step <= 3; ++step) {
        // Spatial fade: whiten 1/3, 2/3, then ALL pixels throughout every
        // glyph, not a top wipe that could erase the whole prompt in frame 1.
        // No temporal gray partial/LUT; full-gray retains its legal waveform.
        for (size_t pixel = 0; pixel < (size_t)EPD_WIDTH * EPD_HEIGHT; ++pixel)
            if (pixel % 3 < step)
                canvas[pixel / 4] |= (uint8_t)(3U << (6 - 2 * (pixel % 4)));
        present_panel(canvas, menu_previous, false);
        if (!display_ok) return false;
        memcpy(menu_previous, canvas, CLOCK_FRAME_SIZE);
    }
    return true; // present_panel already waited and slept, no cache_write.
}

void app_ui_message(const char *title, const char *const *lines, size_t count)
{
    if (!app_ui_owner()) return;
    screen_render(canvas, title, lines, count, -1);
    app_ui_present(canvas, true);
}
