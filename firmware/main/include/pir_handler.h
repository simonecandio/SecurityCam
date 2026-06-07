/*
 * pir_handler.h
 * Gestione sensore PIR con interrupt e debounce
 */
#ifndef PIR_HANDLER_H
#define PIR_HANDLER_H

#include "esp_err.h"
#include <stdbool.h>

// callback per quando il PIR rileva movimento
typedef void (*pir_motion_cb_t)(void *arg);

esp_err_t pir_init(pir_motion_cb_t cb, void *cb_arg);
void pir_deinit(void);
void pir_set_enabled(bool enabled);
bool pir_health_check(void);
bool pir_is_in_cooldown(void);

#endif
