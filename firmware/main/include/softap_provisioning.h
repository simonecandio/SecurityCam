/*
 * softap_provisioning.h
 * Modalità SoftAP con pagina web per configurazione iniziale
 */
#ifndef SOFTAP_PROVISIONING_H
#define SOFTAP_PROVISIONING_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t provisioning_start(void);
void provisioning_stop(void);
bool provisioning_is_configured(void);

#endif
