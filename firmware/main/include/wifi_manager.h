/*
 * wifi_manager.h
 * Connessione WiFi STA con salvataggio credenziali su NVS
 */
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
bool wifi_manager_is_connected(void);
void wifi_manager_get_ip(char *buf, size_t len);
void wifi_manager_stop(void);

#endif
