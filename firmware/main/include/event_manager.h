/*
 * event_manager.h
 * Gestione pipeline evento: PIR -> cattura -> upload/salva -> cooldown
 */
#ifndef EVENT_MANAGER_H
#define EVENT_MANAGER_H

#include "esp_err.h"
#include "camera_handler.h"
#include <stdint.h>

// stati della macchina a stati dell'evento
typedef enum {
    EVT_STATE_IDLE,
    EVT_STATE_TRIGGERED,
    EVT_STATE_CAPTURING,
    EVT_STATE_UPLOADING,
    EVT_STATE_SAVING_LOCAL,
    EVT_STATE_COOLDOWN,
    EVT_STATE_ERROR,
} event_state_t;

// dati di un singolo evento di sicurezza
typedef struct {
    uint32_t         id;
    int64_t          timestamp;
    captured_frame_t frames[5];       // max 5 frame per evento
    int              frame_count;
    bool             uploaded;        // inviato a firebase?
    bool             saved_local;     // salvato su SD?
} security_event_t;

esp_err_t event_manager_init(void);
void event_manager_motion_detected(void);
event_state_t event_manager_get_state(void);
uint32_t event_manager_get_event_count(void);
const security_event_t *event_manager_get_last_event(void);

// fallback: polling mode quando il PIR non funziona
void event_manager_start_polling_mode(void);
void event_manager_stop_polling_mode(void);

#endif
