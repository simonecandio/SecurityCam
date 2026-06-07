/*
 * led_controller.c
 * Gestione LED di stato con pattern di lampeggio.
 * Uso solo il LED su GPIO2 (quello sulla board).
 */
#include "led_controller.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LED";
static led_pattern_t pattern_corrente = LED_OFF;
static TaskHandle_t led_task_handle = NULL;

static void led_task(void *arg)
{
    bool stato = false;

    while (1) {
        switch (pattern_corrente) {

        case LED_IDLE:
            // blink lento: 1s acceso, 2s spento
            gpio_set_level(LED_STATUS_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            gpio_set_level(LED_STATUS_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(2000));
            break;

        case LED_MOTION:
            // blink veloce: 100ms on/off
            stato = !stato;
            gpio_set_level(LED_STATUS_PIN, stato);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case LED_UPLOADING:
            // doppio lampeggio poi pausa
            gpio_set_level(LED_STATUS_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_STATUS_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_STATUS_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_STATUS_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(700));
            break;

        case LED_ERROR:
            // flash rapidissimo
            stato = !stato;
            gpio_set_level(LED_STATUS_PIN, stato);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;

        case LED_PROVISIONING:
            // mezzo secondo on, mezzo off
            stato = !stato;
            gpio_set_level(LED_STATUS_PIN, stato);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case LED_ON:
            gpio_set_level(LED_STATUS_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case LED_OFF:
        default:
            gpio_set_level(LED_STATUS_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }
    }
}

esp_err_t led_controller_init(void)
{
    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << LED_STATUS_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&conf);
    if (err != ESP_OK) return err;

    gpio_set_level(LED_STATUS_PIN, 0);

    xTaskCreate(led_task, "led_task", 2048, NULL, 2, &led_task_handle);

    ESP_LOGI(TAG, "LED inizializzato su GPIO%d", LED_STATUS_PIN);
    return ESP_OK;
}

void led_set_pattern(led_pattern_t p)
{
    if (p != pattern_corrente) {
        pattern_corrente = p;
        // ESP_LOGD(TAG, "Pattern cambiato: %d", p);
    }
}

led_pattern_t led_get_pattern(void)
{
    return pattern_corrente;
}
