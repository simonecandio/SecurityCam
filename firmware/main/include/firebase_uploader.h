/*
 * firebase_uploader.h
 * Upload su Firebase Storage + Firestore via REST API
 * (niente SDK, funziona tutto con HTTP client)
 */
#ifndef FIREBASE_UPLOADER_H
#define FIREBASE_UPLOADER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t firebase_init(void);
esp_err_t firebase_upload_frame(const uint8_t *data, size_t length,
                                uint32_t event_id, int frame_idx);
esp_err_t firebase_write_event(uint32_t event_id, int64_t timestamp,
                               int frame_count, int pir_value);
bool firebase_is_ready(void);
esp_err_t firebase_check_ownership(void);
esp_err_t firebase_self_deauth(void);
esp_err_t firebase_check_remote_config(void);
esp_err_t firebase_upload_status(bool camera_ok, bool pir_ok, bool sd_ok,
                                  bool wifi_ok, bool firebase_ok,
                                  uint32_t uptime_s, uint32_t free_heap,
                                  uint32_t total_events, uint32_t failed_uploads,
                                  const char *state);

#endif