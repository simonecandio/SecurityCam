/*
 * error_monitor.c
 * ============================================================
 * Modulo di monitoraggio periodico delle periferiche.
 * E' il "watchdog" del firmware: gira in un task FreeRTOS
 * dedicato e ogni 5 secondi verifica lo stato di:
 *  - Camera (camera_health_check)
 *  - PIR (pir_health_check)
 *  - SD card (sd_card_is_available)
 *  - WiFi (wifi_manager_is_connected)
 *  - Firebase (firebase_is_ready)
 *
 * Quando rileva un CAMBIO di stato (edge detection), logga
 * l'evento e attiva i fallback automatici:
 *  - PIR scollegato -> attiva polling mode (cattura ogni 30s)
 *  - SD scollegata  -> fallback su buffer RAM
 *  - WiFi caduto    -> sospende upload Firebase
 *
 * Ogni 30 secondi (6 cicli) carica lo stato completo su
 * Firestore cosi' l'app Android puo' vederlo da remoto e
 * controlla se c'e' una configurazione remota in attesa.
 *
 * Ogni 30 secondi fa anche un OWNERSHIP CHECK: verifica che
 * il device sia ancora registrato sull'account utente. Se
 * l'utente l'ha cancellato dall'app mentre era offline, il
 * dispositivo se ne accorge al ritorno online e si auto-
 * deautoriza per non lasciare dati orfani.
 * ============================================================
 */
#include "error_monitor.h"
#include "camera_handler.h"
#include "pir_handler.h"
#include "sd_card_manager.h"
#include "wifi_manager.h"
#include "firebase_uploader.h"
#include "led_controller.h"
#include "event_manager.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "MONITOR";

// === STATO GLOBALE DEL MODULO ===
static system_status_t status = {0};
static TaskHandle_t monitor_task = NULL;
static uint32_t upload_falliti = 0;
static bool polling_mode_attivo = false;
static bool first_check = true;

// === OWNERSHIP CHECK ===
// Counter di "missing" consecutivi rilevati dall'ownership check.
// Solo dopo N conferme consecutive considero il device come davvero
// cancellato. Errori di rete o transitori NON incrementano il counter.
static int ownership_missing_count = 0;
#define OWNERSHIP_MISSING_THRESHOLD 3

// Grace period DOPO IL BOOT durante il quale ignoro completamente i
// "missing" dell'ownership check. Serve per gestire il caso del primo
// boot dopo aver ricevuto setOwnerConfig dall'app: l'app potrebbe non
// aver ancora completato la scrittura del documento Firestore (rete
// lenta, app killata, ecc.) e i primi check ritornerebbero 404
// scatenando un self-deauth ingiustificato.
//
// 5 minuti sono ampiamente sufficienti per qualsiasi propagazione
// realistica di una scrittura Firestore + retry dell'app.
#define OWNERSHIP_GRACE_PERIOD_S 300

/*
 * controlla_tutto()
 * --------------------------------------------------------------
 * Funzione principale del modulo, chiamata ogni 5 secondi dal task.
 * Aggiorna lo stato di tutte le periferiche, attiva fallback se serve,
 * imposta il LED in base alla gravita'. Usa edge detection (logga
 * solo i CAMBI di stato).
 */
static void controlla_tutto(void)
{
    bool cam_prima  = status.camera_ok;
    bool pir_prima  = status.pir_ok;
    bool sd_prima   = status.sd_card_ok;
    bool wifi_prima = status.wifi_ok;

    // === CHECK CAMERA ===
    status.camera_ok = camera_health_check();
    if (!status.camera_ok && (cam_prima || first_check)) {
        ESP_LOGE(TAG, "*** CAMERA SCOLLEGATA! ***");
        ESP_LOGE(TAG, "Gli eventi verranno registrati SENZA foto");
    } else if (status.camera_ok && !cam_prima && !first_check) {
        ESP_LOGI(TAG, "Camera ricollegata! Ripresa cattura immagini.");
    }

    // === CHECK PIR (con polling mode automatico) ===
    status.pir_ok = pir_health_check();
    if (!status.pir_ok && (pir_prima || first_check)) {
        ESP_LOGE(TAG, "*** PIR SCOLLEGATO! ***");
        ESP_LOGE(TAG, "Attivazione POLLING MODE (cattura ogni 30s)");
        if (!polling_mode_attivo) {
            event_manager_start_polling_mode();
            polling_mode_attivo = true;
        }
    } else if (status.pir_ok && !pir_prima && !first_check) {
        ESP_LOGI(TAG, "PIR ricollegato! Disattivazione polling mode.");
        if (polling_mode_attivo) {
            event_manager_stop_polling_mode();
            polling_mode_attivo = false;
        }
    }

    // === CHECK SD CARD ===
    status.sd_card_ok = sd_card_is_available();
    if (!status.sd_card_ok && (sd_prima || first_check)) {
        ESP_LOGW(TAG, "*** SD CARD NON DISPONIBILE! ***");
        ESP_LOGW(TAG, "Fallback: buffer RAM (ultimi 3 eventi)");
    } else if (status.sd_card_ok && !sd_prima && !first_check) {
        ESP_LOGI(TAG, "SD card tornata disponibile!");
    }

    // === CHECK WIFI ===
    status.wifi_ok = wifi_manager_is_connected();
    if (!status.wifi_ok && (wifi_prima || first_check)) {
        ESP_LOGW(TAG, "*** WIFI DISCONNESSO! ***");
        ESP_LOGW(TAG, "Upload sospesi, salvataggio solo locale");
    } else if (status.wifi_ok && !wifi_prima && !first_check) {
        ESP_LOGI(TAG, "WiFi riconnesso! Upload ripresi.");
    }

    // Firebase dipende dal WiFi
    status.firebase_ok = status.wifi_ok && firebase_is_ready();

    // === STATISTICHE DI SISTEMA ===
    status.uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000);
    status.free_heap = esp_get_free_heap_size();
    status.total_events = event_manager_get_event_count();
    status.failed_uploads = upload_falliti;

    // === LED PATTERN BASATO SULLA GRAVITA' ===
    event_state_t evt_state = event_manager_get_state();
    if (evt_state == EVT_STATE_IDLE) {
        if (!status.camera_ok && !status.pir_ok) {
            led_set_pattern(LED_ERROR);
        } else if (!status.camera_ok || (!status.wifi_ok && !status.sd_card_ok)) {
            led_set_pattern(LED_ERROR);
        } else if (!status.pir_ok || !status.wifi_ok || !status.sd_card_ok) {
            led_set_pattern(LED_UPLOADING); // warning
        } else {
            led_set_pattern(LED_IDLE);
        }
    }

    // === LOG DI RIEPILOGO ===
    ESP_LOGI(TAG, "[STATO] cam=%s pir=%s sd=%s wifi=%s fb=%s | heap=%luKB evt=%lu up=%lus%s",
             status.camera_ok   ? "OK" : "NO",
             status.pir_ok      ? "OK" : "NO",
             status.sd_card_ok  ? "OK" : "NO",
             status.wifi_ok     ? "OK" : "NO",
             status.firebase_ok ? "OK" : "NO",
             (unsigned long)(status.free_heap / 1024),
             (unsigned long)status.total_events,
             (unsigned long)status.uptime_seconds,
             polling_mode_attivo ? " [POLLING]" : "");

    first_check = false;
}

/*
 * monitor_task_fn()
 * --------------------------------------------------------------
 * Task FreeRTOS principale. Loop infinito che ogni 5 secondi fa il
 * check delle periferiche, e ogni 30 secondi (6 cicli) fa l'upload
 * dello status su Firestore + check config remota + ownership check.
 */
static void monitor_task_fn(void *arg)
{
    // attesa iniziale di 2 secondi per rilevare problemi al boot
    vTaskDelay(pdMS_TO_TICKS(2000));

    int check_counter = 0;

    while (1) {
        controlla_tutto();

        check_counter++;
        if (check_counter >= 3 && status.wifi_ok) {
            check_counter = 0;

            const char *state_str = "unknown";
            switch (event_manager_get_state()) {
                case EVT_STATE_IDLE:         state_str = "idle"; break;
                case EVT_STATE_TRIGGERED:    state_str = "triggered"; break;
                case EVT_STATE_CAPTURING:    state_str = "capturing"; break;
                case EVT_STATE_UPLOADING:    state_str = "uploading"; break;
                case EVT_STATE_SAVING_LOCAL: state_str = "saving_local"; break;
                case EVT_STATE_COOLDOWN:     state_str = "cooldown"; break;
                case EVT_STATE_ERROR:        state_str = "error"; break;
            }

            // upload status su Firestore (status/current)
            firebase_upload_status(
                status.camera_ok, status.pir_ok, status.sd_card_ok,
                status.wifi_ok, status.firebase_ok,
                status.uptime_seconds, status.free_heap,
                status.total_events, status.failed_uploads,
                state_str
            );

            // check se l'app ha lasciato una configurazione in pending
            // (cooldown/frame_count/jpeg_quality o factory_reset)
            firebase_check_remote_config();

            // === OWNERSHIP CHECK ===
            // Verifico che il device sia ancora registrato sull'account.
            // Se l'utente l'ha cancellato dall'app mentre ero offline,
            // me ne accorgo e mi auto-deautorizzo.
            //
            // GRACE PERIOD: nei primi 5 minuti dopo il boot ignoro
            // completamente i 404. Serve per il primo boot post-
            // setOwnerConfig: l'app potrebbe non aver ancora completato
            // la scrittura del documento Firestore (rete lenta, app
            // killata in mezzo, race condition) e i primi check
            // ritornerebbero 404 scatenando un self-deauth ingiustificato.
            esp_err_t own_res = firebase_check_ownership();
            if (own_res == ESP_ERR_NOT_FOUND) {
                if (status.uptime_seconds < OWNERSHIP_GRACE_PERIOD_S) {
                    ESP_LOGW(TAG, "Ownership: 404 ma sono nel grace period "
                                  "(%lus / %ds), ignoro",
                             (unsigned long)status.uptime_seconds,
                             OWNERSHIP_GRACE_PERIOD_S);
                } else {
                    ownership_missing_count++;
                    ESP_LOGW(TAG, "Ownership: device non trovato su Firestore "
                                  "(%d/%d conferme)",
                             ownership_missing_count,
                             OWNERSHIP_MISSING_THRESHOLD);

                    if (ownership_missing_count >= OWNERSHIP_MISSING_THRESHOLD) {
                        ESP_LOGE(TAG, "Device confermato cancellato dall'utente. "
                                      "Avvio self-deauth.");
                        // questa chiamata NON ritorna (esp_restart interno)
                        firebase_self_deauth();
                    }
                }
            } else if (own_res == ESP_OK) {
                if (ownership_missing_count > 0) {
                    ESP_LOGI(TAG, "Ownership: device ritrovato, reset counter");
                }
                ownership_missing_count = 0;
            }
            // ESP_FAIL / ESP_ERR_INVALID_STATE: NON tocco il contatore
            // (potrebbe essere solo un problema di rete temporaneo)
        }

        vTaskDelay(pdMS_TO_TICKS(ERROR_CHECK_INTERVAL_MS));
    }
}

/*
 * error_monitor_init()
 * --------------------------------------------------------------
 * Inizializza il modulo creando il task FreeRTOS.
 * Chiamata da main.c al passo [10].
 */
esp_err_t error_monitor_init(void)
{
    memset(&status, 0, sizeof(status));
    status.camera_ok  = false;
    status.pir_ok     = false;
    status.sd_card_ok = false;
    status.wifi_ok    = false;
    first_check       = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        monitor_task_fn,
        "monitor",
        8192,
        NULL,
        3,                // priorita' bassa (event_manager ha 6, WiFi ha 23)
        &monitor_task,
        0                 // core 0
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Errore creazione task monitor");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Error monitor avviato (check ogni %ds)",
             ERROR_CHECK_INTERVAL_MS / 1000);
    return ESP_OK;
}

system_status_t error_monitor_get_status(void)
{
    return status;
}

void error_monitor_check_now(void)
{
    controlla_tutto();
}

void error_monitor_record_upload_failure(void)
{
    upload_falliti++;
}
