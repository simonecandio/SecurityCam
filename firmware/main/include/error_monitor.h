/*
 * error_monitor.h
 * Controllo periodico dello stato di tutte le periferiche
 * (camera, PIR, SD card, WiFi, Firebase)
 */
#ifndef ERROR_MONITOR_H
#define ERROR_MONITOR_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool camera_ok;
    bool pir_ok;
    bool sd_card_ok;
    bool wifi_ok;
    bool firebase_ok;
    uint32_t uptime_seconds;
    uint32_t free_heap;
    uint32_t total_events;
    uint32_t failed_uploads;
} system_status_t;

esp_err_t error_monitor_init(void);
system_status_t error_monitor_get_status(void);
void error_monitor_check_now(void);
void error_monitor_record_upload_failure(void);

#endif
