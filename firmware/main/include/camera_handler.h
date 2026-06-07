/*
 * camera_handler.h
 * Gestione camera OV2640
 */
#ifndef CAMERA_HANDLER_H
#define CAMERA_HANDLER_H

#include "esp_camera.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// struttura per un frame catturato
typedef struct {
    uint8_t *data;      // buffer JPEG (va liberato con free!)
    size_t   length;    // dimensione in byte
    int64_t  timestamp; // timestamp cattura (ms)
} captured_frame_t;

esp_err_t camera_init(void);
void camera_deinit(void);
esp_err_t camera_capture_frame(captured_frame_t *frame);
int camera_capture_burst(captured_frame_t *frames, int count, int interval_ms);
bool camera_health_check(void);

#endif
