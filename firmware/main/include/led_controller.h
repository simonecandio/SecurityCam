/*
 * led_controller.h
 * Controllo LED di stato con pattern diversi
 */
#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include "esp_err.h"

typedef enum {
    LED_IDLE,           // blink lento - sistema in attesa
    LED_MOTION,         // blink veloce - movimento rilevato
    LED_UPLOADING,      // doppio blink - upload in corso
    LED_ERROR,          // flash rapidissimo - errore
    LED_PROVISIONING,   // alternato - modalità config
    LED_OFF,
    LED_ON,
} led_pattern_t;

esp_err_t led_controller_init(void);
void led_set_pattern(led_pattern_t pattern);
led_pattern_t led_get_pattern(void);

#endif
