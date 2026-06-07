/*
 * pir_handler.c
 * Gestione PIR con interrupt hardware, debounce software e cooldown.
 * 
 * Il PIR (tipo HC-SR501) genera un segnale HIGH quando rileva movimento.
 * 
 * Il cooldown serve a non generare 100 eventi al secondo se qualcuno
 * cammina davanti alla camera per un minuto.
 */
#include "pir_handler.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "PIR";

static pir_motion_cb_t s_callback   = NULL;
static void           *s_cb_arg     = NULL;
static bool            s_enabled    = true;
static int64_t         s_last_trigger_ms = 0;
static QueueHandle_t   s_isr_queue  = NULL;
static TaskHandle_t    s_task       = NULL;

// ISR - deve essere velocissimo, mando solo un segnale alla coda
static void IRAM_ATTR pir_isr(void *arg)
{
    uint32_t dummy = 1;
    xQueueSendFromISR(s_isr_queue, &dummy, NULL);
}

// Task che processa gli interrupt con debounce e cooldown
static void pir_task(void *arg)
{
    uint32_t evt;
    while (1) {
        if (xQueueReceive(s_isr_queue, &evt, portMAX_DELAY)) {
            if (!s_enabled) continue;

            int64_t now = esp_timer_get_time() / 1000;

            // debounce: ignoro trigger troppo vicini
            if ((now - s_last_trigger_ms) < PIR_DEBOUNCE_MS) {
                continue;
            }

            // cooldown: ignoro trigger durante il periodo di cooldown
            if ((now - s_last_trigger_ms) < PIR_COOLDOWN_MS && s_last_trigger_ms != 0) {
                ESP_LOGD(TAG, "Cooldown attivo, ignoro (%lld ms rimasti)",
                         PIR_COOLDOWN_MS - (now - s_last_trigger_ms));
                continue;
            }

            // verifico che il pin sia ancora HIGH dopo 50ms
            // (per escludere glitch elettrici)
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(PIR_GPIO_PIN) == 0) {
                ESP_LOGD(TAG, "Falso allarme (pin tornato LOW)");
                continue;
            }

            s_last_trigger_ms = now;
            ESP_LOGI(TAG, "=== MOVIMENTO RILEVATO! === (t=%lld ms)", now);

            if (s_callback) {
                s_callback(s_cb_arg);
            }
        }
    }
}

esp_err_t pir_init(pir_motion_cb_t cb, void *cb_arg)
{
    s_callback = cb;
    s_cb_arg   = cb_arg;

    // configuro il GPIO come input con pull-down
    // il pull-down serve per avere un livello definito quando il PIR
    // è scollegato (legge 0 invece di flottare)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIR_GPIO_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,  // rising edge = movimento
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Errore configurazione GPIO: %s", esp_err_to_name(err));
        return err;
    }

    // coda per comunicazione ISR -> task
    s_isr_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!s_isr_queue) {
        ESP_LOGE(TAG, "Errore creazione coda ISR");
        return ESP_ERR_NO_MEM;
    }

    // installo il servizio ISR per GPIO
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE = già installato, va bene
        ESP_LOGE(TAG, "Errore installazione servizio ISR: %s", esp_err_to_name(err));
        return err;
    }

    err = gpio_isr_handler_add(PIR_GPIO_PIN, pir_isr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Errore aggiunta handler ISR: %s", esp_err_to_name(err));
        return err;
    }

    // creo il task di elaborazione sul core 1
    BaseType_t ret = xTaskCreatePinnedToCore(
        pir_task, "pir_task", 3072, NULL, 5, &s_task, 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Errore creazione task PIR");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PIR inizializzato su GPIO%d (cooldown=%ds, debounce=%dms)",
             PIR_GPIO_PIN, PIR_COOLDOWN_MS/1000, PIR_DEBOUNCE_MS);
    return ESP_OK;
}

void pir_deinit(void)
{
    gpio_isr_handler_remove(PIR_GPIO_PIN);
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_isr_queue) {
        vQueueDelete(s_isr_queue);
        s_isr_queue = NULL;
    }
    ESP_LOGI(TAG, "PIR deinizializzato");
}

void pir_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "PIR %s", enabled ? "ABILITATO" : "DISABILITATO");
}

bool pir_health_check(void)
{
    /*
     * Tecnica per verificare se il PIR è collegato:
     * 
     * 1. Attivo il pull-UP interno sul pin (normalmente ha pull-DOWN)
     * 2. Aspetto un attimo che si stabilizzi
     * 3. Leggo il pin
     * 4. Rimetto il pull-DOWN
     * 5. Aspetto un attimo
     * 6. Leggo di nuovo
     * 
     * Se il PIR è COLLEGATO: il suo output (open-drain o push-pull) 
     *   domina i pull interni dell'ESP32 (che sono deboli, ~45kΩ).
     *   Il pin legge lo stesso valore in entrambi i casi.
     * 
     * Se il PIR è SCOLLEGATO: il pin flotta e segue i pull interni.
     *   Con pull-up legge 1, con pull-down legge 0.
     *   Se i due valori sono diversi → pin scollegato!
     */
    
    // disabilito temporaneamente l'interrupt per non generare falsi trigger
    gpio_intr_disable(PIR_GPIO_PIN);
    
    // test con pull-up
    gpio_set_pull_mode(PIR_GPIO_PIN, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(10));
    int val_pullup = gpio_get_level(PIR_GPIO_PIN);
    
    // test con pull-down
    gpio_set_pull_mode(PIR_GPIO_PIN, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(10));
    int val_pulldown = gpio_get_level(PIR_GPIO_PIN);
    
    // ripristino configurazione normale (pull-down + interrupt)
    gpio_set_pull_mode(PIR_GPIO_PIN, GPIO_PULLDOWN_ONLY);
    gpio_intr_enable(PIR_GPIO_PIN);
    
    // se i valori sono diversi, il pin sta seguendo i pull interni
    // quindi non c'è nessun dispositivo collegato
    if (val_pullup != val_pulldown) {
        ESP_LOGE(TAG, "PIR SCOLLEGATO! (pullup=%d, pulldown=%d)", val_pullup, val_pulldown);
        return false;
    }
    
    ESP_LOGD(TAG, "PIR OK (pullup=%d, pulldown=%d)", val_pullup, val_pulldown);
    return true;
}

bool pir_is_in_cooldown(void)
{
    if (s_last_trigger_ms == 0) return false;
    int64_t now = esp_timer_get_time() / 1000;
    return (now - s_last_trigger_ms) < PIR_COOLDOWN_MS;
}
