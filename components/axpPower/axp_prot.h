#ifndef AXP_PROT_H
#define AXP_PROT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum axp2101_pwr_tab {
    DC1 = 1,
    ALDO1,
    ALDO2,
    ALDO3,
    ALDO4,
} axp2101_pwr_tab_t;

#ifdef __cplusplus
extern "C" {
#endif


void axp_init(void);
// Owner task only. Accepts 128/512/1000/2000ms; preserves power-off timing.
bool axp_set_power_on_time(unsigned ms);
// Owner-only checked I2C operations. Hard fallback uses 10s, not 4s.
bool axp_configure_power_key_fallback(void); // 10s hard shutdown stays enabled.
bool axp_power_key_begin(void); // Enable IRQs and discard boot residue.
bool axp_power_key_read(uint8_t *events); // INTSTS2 low nibble, observed W1C.
bool axp_power_off_checked(void); // Command accepted, NOT proof power is gone.
int get_battery_power(void);
bool enapwrstate(uint8_t tab);
bool disapwrstate(uint8_t tab);
void axp_pwr_off(void);
bool get_usb_connected(void);

#ifdef __cplusplus
}
#endif

#endif