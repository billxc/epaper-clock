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
int get_battery_power(void);
bool enapwrstate(uint8_t tab);
bool disapwrstate(uint8_t tab);
void axp_pwr_off(void);
bool get_usb_connected(void);

#ifdef __cplusplus
}
#endif

#endif