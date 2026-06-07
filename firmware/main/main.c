/*
 * main.c
 * Entry point del firmware Security Camera.
 * 
 * Il sistema è progettato per degradare gracefully:
 * ogni componente che fallisce ha un'alternativa.
 * Non si crasha mai, si adatta alla situazione.
 */
#include "board_config.h"
#include "camera_handler.h"
#include "pir_handler.h"
#include "wifi_manager.h"
#include "softap_provisioning.h"
#include "http_server.h"
#include "sd_card_manager.h"
#include "led_controller.h"
#include "firebase_uploader.h"
#include "event_manager.h"
#include "error_monitor.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cert_manager.h"
#include "tflite_classifier.h"

static const char *TAG = "MAIN";

// callback chiamata quando il PIR rileva movimento
static void on_motion(void *arg)
{
    ESP_LOGI(TAG, ">>> Movimento rilevato! Invio a event manager...");
    event_manager_motion_detected();
}

void app_main(void)
{
    ESP_LOGI(TAG, "===================================");
    ESP_LOGI(TAG, "  Security Camera - ESP32-S3");
    ESP_LOGI(TAG, "  Progetto esame IoT");
    ESP_LOGI(TAG, "  Firmware v1.0.0");
    ESP_LOGI(TAG, "===================================");
    //Monitor pulito
    esp_log_level_set("cam_hal", ESP_LOG_ERROR);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_NONE);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_NONE);
    esp_log_level_set("cam_hal", ESP_LOG_ERROR);
    esp_log_level_set("sccb-ng", ESP_LOG_NONE);
    esp_log_level_set("s3 ll_cam", ESP_LOG_NONE);
    esp_log_level_set("pp", ESP_LOG_NONE);
    esp_log_level_set("net80211", ESP_LOG_NONE);
    esp_log_level_set("esp_image", ESP_LOG_NONE);
    esp_log_level_set("sleep_gpio", ESP_LOG_NONE);
    esp_log_level_set("octal_psram", ESP_LOG_NONE);
    esp_log_level_set("MSPI Timing", ESP_LOG_NONE);
    esp_log_level_set("spi_flash", ESP_LOG_NONE);
    esp_log_level_set("efuse_init", ESP_LOG_NONE);
    esp_log_level_set("heap_init", ESP_LOG_NONE);
    esp_log_level_set("cpu_start", ESP_LOG_NONE);
    esp_log_level_set("app_init", ESP_LOG_NONE);
    esp_log_level_set("esp_psram", ESP_LOG_WARN);
    esp_log_level_set("ov2640", ESP_LOG_NONE);
    // 1. init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: cancello e reinizializzo...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1] NVS OK");

    // 2. LED
    ret = led_controller_init();
    if (ret != ESP_OK) {
        // FALLBACK LED: se il LED non funziona, loggo su seriale
        // il sistema continua, l'utente vede lo stato dal monitor
        ESP_LOGW(TAG, "[2] LED non disponibile - stato solo via seriale/HTTP");
    } else {
        led_set_pattern(LED_ON);
        ESP_LOGI(TAG, "[2] LED OK");
    }

    // 3. controllo se il dispositivo è già stato configurato
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (!provisioning_is_configured()) {
        ESP_LOGW(TAG, "[3] Dispositivo NON configurato");
        led_set_pattern(LED_PROVISIONING);
        ESP_ERROR_CHECK(provisioning_start());

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "  ============================================");
        ESP_LOGI(TAG, "  MODALITA' CONFIGURAZIONE");
        ESP_LOGI(TAG, "  1. Connettersi al WiFi: %s", SOFTAP_SSID);
        ESP_LOGI(TAG, "     Password: %s", SOFTAP_PASS);
        ESP_LOGI(TAG, "  2. Aprire http://192.168.4.1");
        ESP_LOGI(TAG, "  3. Inserire dati WiFi e Firebase");
        ESP_LOGI(TAG, "  4. Il dispositivo si riavvia da solo");
        ESP_LOGI(TAG, "  ============================================");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            ESP_LOGI(TAG, "In attesa di configurazione...");
        }
    }

    ESP_LOGI(TAG, "[3] Dispositivo configurato, avvio normale");

    // 4. camera
    ret = camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[4] Camera FALLITA");
        ESP_LOGW(TAG, "    -> FALLBACK: eventi registrati senza immagini");
    } else {
        ESP_LOGI(TAG, "[4] Camera OK");
    }

    //4.5 TFLite classifier 
    if (tflite_classifier_init() == ESP_OK) {
        ESP_LOGI(TAG, "[4.5] TFLite classificatore OK");
    } else {
        ESP_LOGW(TAG, "[4.5] TFLite non disponibile, filtro disabilitato");
    }

    // 5. SD card
    ret = sd_card_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[5] SD card non disponibile");
        ESP_LOGW(TAG, "    -> FALLBACK: salvataggio in buffer RAM");
    } else {
        uint64_t free_mb = sd_card_get_free_bytes() / (1024*1024);
        ESP_LOGI(TAG, "[5] SD card OK (%llu MB liberi)", (unsigned long long)free_mb);
    }

    // 6. WiFi
    ret = wifi_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[6] WiFi NON connesso");
        ESP_LOGW(TAG, "    -> FALLBACK: salvataggio solo locale (SD/RAM)");
    } else {
        char ip[16];
        wifi_manager_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "[6] WiFi OK - IP: %s", ip);
    }

    // 7. Firebase
    ret = firebase_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "[7] Firebase non configurato");
    } else {
        ESP_LOGI(TAG, "[7] Firebase OK");
    }

    // 8. Event manager (deve essere prima del PIR!)
    ESP_ERROR_CHECK(event_manager_init());
    ESP_LOGI(TAG, "[8] Event manager OK");

    // 9. PIR
    ret = pir_init(on_motion, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[9] PIR FALLITO: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "    -> FALLBACK: polling mode (cattura ogni 30s)");
        event_manager_start_polling_mode();
    } else {
        ESP_LOGI(TAG, "[9] PIR OK (GPIO%d)", PIR_GPIO_PIN);
    }

    // 10. Error monitor
    ESP_ERROR_CHECK(error_monitor_init());
    ESP_LOGI(TAG, "[10] Error monitor OK");

    // 11. Server HTTP (solo se c'è il WiFi)
    if (wifi_manager_is_connected()) {
        ESP_LOGI(TAG, "[10.5] Certificati TLS...");
        if (cert_manager_init() == ESP_OK) {
            ESP_LOGI(TAG, "[10.5] Certificati TLS OK");
        } else {
            ESP_LOGW(TAG, "[10.5] Certificati TLS falliti, HTTP senza cifratura");
        }
        ret = http_server_start();
        if (ret == ESP_OK) {
            char ip[16];
            wifi_manager_get_ip(ip, sizeof(ip));
            ESP_LOGI(TAG, "[11] Server HTTP su http://%s/", ip);
        }
    } else {
        ESP_LOGW(TAG, "[11] Server HTTP saltato (no WiFi)");
        ESP_LOGW(TAG, "     -> HTTP disponibile quando WiFi si riconnette");
    }

    // 12. tutto pronto
    led_set_pattern(LED_IDLE);

  // leggo valori effettivi da NVS (se configurati da remoto)
    uint32_t actual_cooldown = PIR_COOLDOWN_MS;
    uint8_t actual_frames = CAPTURE_FRAME_COUNT;
    uint8_t actual_quality = CAPTURE_QUALITY;
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            nvs_get_u32(nvs, NVS_KEY_COOLDOWN, &actual_cooldown);
            nvs_get_u8(nvs, NVS_KEY_FRAME_COUNT, &actual_frames);
            nvs_get_u8(nvs, NVS_KEY_QUALITY, &actual_quality);
            nvs_close(nvs);
        }
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  ============================================");
    ESP_LOGI(TAG, "  SISTEMA ATTIVO - MONITORAGGIO IN CORSO");
    ESP_LOGI(TAG, "  PIR su GPIO%d", PIR_GPIO_PIN);
    ESP_LOGI(TAG, "  Frame per evento: %d", actual_frames);
    ESP_LOGI(TAG, "  Cooldown: %lu secondi", (unsigned long)(actual_cooldown / 1000));
    ESP_LOGI(TAG, "  Qualita JPEG: %d", actual_quality);
    ESP_LOGI(TAG, "  Heap libero: %lu KB", (unsigned long)(esp_get_free_heap_size()/1024));
    ESP_LOGI(TAG, "  ============================================");
    ESP_LOGI(TAG, "");

}
