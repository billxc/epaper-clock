#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Blocking BLE-only Wi-Fi provisioning (ESP-IDF 5.5.2, Security1 + fresh PoP).
 * Returns true ONLY if credentials which obtained an IP were saved successfully
 * through network_save_config(). Failure/timeout does not save or clear clock_net.
 * The session has a five-minute budget, including a five-second teardown reserve;
 * IDF driver teardown and EPD busy waits must themselves complete normally.
 *
 * Call from the application task, NEVER an event-loop/BLE callback. Serialize all
 * network/display operations; enter with Wi-Fi/BT deinitialized and no concurrent
 * ADC/RF use (fresh PoP generation briefly enables the hardware entropy source).
 * NVS, network_init(), and epaper_port_init() must already have run.
 *
 * Required network integration:
 *   esp_err_t network_prepare_provisioning(void);
 *     Initialize driver + STA, but do NOT start/connect Wi-Fi. No reconnect
 *     handler may compete with the manager. Leave cleanup possible on failure.
 *   void network_finish_provisioning(void);
 *     Unconditionally stop/deinitialize Wi-Fi, even though network's normal
 *     wifi_running flag was not set by manager; drain events/reset driver state.
 *
 * This module owns manager init/start/stop/deinit, connection attempts, and Wi-Fi
 * persistent-config cleanup. After manager deinit, it calls esp_wifi_restore()
 * and restores RAM storage BEFORE network_finish_provisioning(). Neither hook
 * may erase clock_net. No other code may deinit the manager on WIFI_PROV_END or
 * free BT memory/enable keep-BLE-on. Successful runs may be repeated this boot.
 *
 * Display integration (implemented by display.c):
 *   void provision_render(uint8_t *frame, const char *service_name,
 *                         const char *pop);
 * Called once with an allocated CLOCK_FRAME_SIZE buffer, then full EPD refresh
 * and sleep. The caller must force a full clock refresh after EVERY return;
 * this screen is deliberately not written to the clock's partial-refresh cache.
 * Never log the rendered PoP, QR payload, or Wi-Fi credentials.
 */
bool provision_run(void);

#ifdef __cplusplus
}
#endif
