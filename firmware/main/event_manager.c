/*
 * event_manager.c
 * Cuore del sistema: gestisce la pipeline completa
 * PIR trigger -> filtro TFLite -> cattura burst -> upload firebase -> salvataggio SD -> cooldown
 * 
 * MODALITA' FALLBACK (degradazione graceful):
 * - Senza PIR: passa in "polling mode", cattura ogni 30 secondi
 * - Senza Camera: registra comunque l'evento con solo timestamp (no immagini)
 * - Senza SD: salva in RAM (buffer circolare ultimi 3 eventi)
 * - Senza WiFi: salva tutto su SD, retry periodico
 * - Senza WiFi E SD: buffer RAM, dati persi al riavvio
 * - Senza TFLite: nessun filtro, uploada tutto (fail-safe)
 */
#include "event_manager.h"
#include "camera_handler.h"
#include "firebase_uploader.h"
#include "sd_card_manager.h"
#include "led_controller.h"
#include "error_monitor.h"
#include "wifi_manager.h"
#include "board_config.h"
#include "tflite_classifier.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include "nvs.h"

static const char *TAG = "EVENT";

static event_state_t    stato_corrente = EVT_STATE_IDLE;
static QueueHandle_t    coda_eventi    = NULL;
static TaskHandle_t     task_handle    = NULL;
static TaskHandle_t     polling_task   = NULL;  // task per polling mode (senza PIR)
static uint32_t         contatore_eventi = 0;
static security_event_t ultimo_evento  = {0};

// buffer RAM per quando non c'è né WiFi né SD
// salvo gli ultimi 3 eventi (solo metadati, non i frame per non esaurire la RAM)
#define RAM_BUFFER_SIZE 3
static security_event_t ram_buffer[RAM_BUFFER_SIZE];
static int ram_buffer_idx = 0;
static int ram_buffer_count = 0;

static void libera_frames(security_event_t *evt)
{
    for (int i = 0; i < evt->frame_count; i++) {
        if (evt->frames[i].data) {
            free(evt->frames[i].data);
            evt->frames[i].data = NULL;
        }
    }
}

static char *crea_json_evento(const security_event_t *evt)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "event_id", evt->id);
    cJSON_AddNumberToObject(root, "timestamp", (double)evt->timestamp);
    cJSON_AddNumberToObject(root, "frame_count", evt->frame_count);
    cJSON_AddBoolToObject(root, "uploaded", evt->uploaded);
    cJSON_AddBoolToObject(root, "saved_local", evt->saved_local);

    cJSON *sizes = cJSON_AddArrayToObject(root, "frame_sizes");
    for (int i = 0; i < evt->frame_count; i++) {
        cJSON_AddItemToArray(sizes, cJSON_CreateNumber(evt->frames[i].length));
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

// salva evento nel buffer RAM (circolare)
static void salva_in_ram(const security_event_t *evt)
{
    // libero eventuali frame vecchi in questa posizione
    libera_frames(&ram_buffer[ram_buffer_idx]);
    
    // copio i metadati ma NON i frame (troppa RAM)
    ram_buffer[ram_buffer_idx].id = evt->id;
    ram_buffer[ram_buffer_idx].timestamp = evt->timestamp;
    ram_buffer[ram_buffer_idx].frame_count = evt->frame_count;
    ram_buffer[ram_buffer_idx].uploaded = evt->uploaded;
    ram_buffer[ram_buffer_idx].saved_local = false;
    memset(ram_buffer[ram_buffer_idx].frames, 0, sizeof(ram_buffer[ram_buffer_idx].frames));
    
    ram_buffer_idx = (ram_buffer_idx + 1) % RAM_BUFFER_SIZE;
    if (ram_buffer_count < RAM_BUFFER_SIZE) ram_buffer_count++;
    
    ESP_LOGW(TAG, "[RAM] Evento salvato in buffer RAM (%d/%d)", 
             ram_buffer_count, RAM_BUFFER_SIZE);
}

// ===== Elaborazione di un singolo evento di movimento =====
static void elabora_evento(void)
{

    security_event_t evt = {0};
    evt.id = contatore_eventi;
    evt.timestamp = esp_timer_get_time() / 1000;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "============================================");
     ESP_LOGI(TAG, "  Movimento rilevato - Analisi in corso...");
    ESP_LOGI(TAG, "============================================");

    // --- FASE 3: cattura + filtro TFLite + burst ---
    stato_corrente = EVT_STATE_CAPTURING;
    led_set_pattern(LED_MOTION);

    if (camera_health_check()) {

        // ===== PRE-FILTRO TFLITE =====
        // Cattura UN frame di test e lo passa al micro classificatore.
        // Se nessuna categoria monitorata supera la soglia, l'evento
        // viene scartato (falso positivo del PIR: ombra, tenda, ecc.)
        // e si risparmia l'upload su Firebase.
        //
        // In caso di errore (classificatore non inizializzato, frame
        // corrotto, ecc.) il filtro lascia passare tutto (fail-safe:
        // meglio un falso positivo che un evento perso).
        bool pass_filter = true;  // default: uploada (fail-safe)

        // Warm-up: scarta il primo frame (spesso corrotto NO-SOI)
        captured_frame_t warmup = {0};
        camera_capture_frame(&warmup);
        if (warmup.data) free(warmup.data);
        vTaskDelay(pdMS_TO_TICKS(100));

        // Cattura il frame reale per il filtro (con retry)
        captured_frame_t test_frame = {0};
        esp_err_t cap_err = camera_capture_frame(&test_frame);
        if ((cap_err != ESP_OK || test_frame.data == NULL) && test_frame.data == NULL) {
            vTaskDelay(pdMS_TO_TICKS(200));
            cap_err = camera_capture_frame(&test_frame);
        }
        
        if (cap_err == ESP_OK && test_frame.data != NULL) {
            classify_result_t clf_result;
            esp_err_t clf_err = tflite_classify_frame(
                test_frame.data, test_frame.length, &clf_result);

            if (clf_err == ESP_OK) {
                // Soglia 0.5: stessa usata nell'app Android
                pass_filter = tflite_should_upload(&clf_result, 0.5f);
            } else {
                ESP_LOGW(TAG, "[FILTRO] Classificazione fallita, procedo con upload");
            }

            // Libera il frame di test (il burst ne catturera' di nuovi)
            free(test_frame.data);
            test_frame.data = NULL;
        } else {
            ESP_LOGW(TAG, "[FILTRO] Cattura frame test fallita, procedo con upload");
        }

        if (!pass_filter) {
            ESP_LOGW(TAG, "[FILTRO] Evento #%lu scartato (nessuna categoria rilevata)",
                     (unsigned long)evt.id);
            led_set_pattern(LED_IDLE);
            stato_corrente = EVT_STATE_COOLDOWN;
            // Cooldown normale anche dopo SKIP per evitare raffiche
            uint32_t skip_cooldown = PIR_COOLDOWN_MS;
            {
                nvs_handle_t nvs;
                if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
                    nvs_get_u32(nvs, NVS_KEY_COOLDOWN, &skip_cooldown);
                    nvs_close(nvs);
                }
            }
            ESP_LOGI(TAG, "[COOLDOWN] Attesa %lu secondi...",
                     (unsigned long)(skip_cooldown / 1000));
            vTaskDelay(pdMS_TO_TICKS(skip_cooldown));
            stato_corrente = EVT_STATE_IDLE;
            return;
        }

        // ===== Filtro passato: burst completo =====
         contatore_eventi++;

    // salvo il contatore in NVS per persistenza al riavvio
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u32(nvs, "evt_counter", contatore_eventi);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    evt.id = contatore_eventi;
        ESP_LOGI(TAG, "[FILTRO] Categoria rilevata! Evento #%lu - Procedo con cattura burst",
                 (unsigned long)evt.id);
        ESP_LOGI(TAG, "[CATTURA] Acquisizione %d frame (intervallo %dms)...",
                 CAPTURE_FRAME_COUNT, CAPTURE_INTERVAL_MS);

        evt.frame_count = camera_capture_burst(
            evt.frames, CAPTURE_FRAME_COUNT, CAPTURE_INTERVAL_MS
        );

        if (evt.frame_count == 0) {
            ESP_LOGE(TAG, "[CATTURA] Cattura fallita nonostante health check OK");
        } else {
            ESP_LOGI(TAG, "[CATTURA] %d frame acquisiti", evt.frame_count);
        }
    } else {
         // camera non disponibile: registro evento senza foto
        contatore_eventi++;
        {
            nvs_handle_t nvs;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_u32(nvs, "evt_counter", contatore_eventi);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
        }
        evt.id = contatore_eventi;
        ESP_LOGW(TAG, "[CATTURA] Camera non disponibile! Evento #%lu solo metadati",
                 (unsigned long)evt.id);
        evt.frame_count = 0;
    }

    // --- FASE 4: upload su Firebase (se possibile) ---
    bool upload_riuscito = false;

    if (wifi_manager_is_connected() && firebase_is_ready()) {
        stato_corrente = EVT_STATE_UPLOADING;
        led_set_pattern(LED_UPLOADING);

        // upload frame solo se ce ne sono
        if (evt.frame_count > 0) {
            ESP_LOGI(TAG, "[UPLOAD] Invio %d frame a Firebase...", evt.frame_count);

            int uploadati = 0;
            for (int i = 0; i < evt.frame_count; i++) {
                if (evt.frames[i].data) {
                    esp_err_t err = firebase_upload_frame(
                        evt.frames[i].data, evt.frames[i].length, evt.id, i
                    );
                    if (err == ESP_OK) {
                        uploadati++;
                    } else {
                        ESP_LOGW(TAG, "[UPLOAD] Frame %d fallito", i);
                        error_monitor_record_upload_failure();
                    }
                }
            }
            ESP_LOGI(TAG, "[UPLOAD] Frame uploadati: %d/%d", uploadati, evt.frame_count);
        }

        // scrivo sempre i metadati anche senza frame
        // cosi l'app sa che c'è stato un evento
        esp_err_t err = firebase_write_event(
            evt.id, evt.timestamp, evt.frame_count, 1
        );
        if (err == ESP_OK) {
            upload_riuscito = true;
            evt.uploaded = true;
            ESP_LOGI(TAG, "[UPLOAD] Metadati evento scritti su Firestore");
        }
    } else {
        ESP_LOGW(TAG, "[UPLOAD] WiFi o Firebase non disponibile, salto upload");
    }

    // --- Salvataggio su SD card (sempre, come backup) ---
    if (sd_card_is_available()) {
        stato_corrente = EVT_STATE_SAVING_LOCAL;

        // salvo i frame solo se ci sono
        if (evt.frame_count > 0) {
            ESP_LOGI(TAG, "[SD] Salvataggio %d frame...", evt.frame_count);
            for (int i = 0; i < evt.frame_count; i++) {
                if (evt.frames[i].data) {
                    sd_card_save_frame(evt.frames[i].data, evt.frames[i].length,
                                       evt.id, i);
                }
            }
        }

        // salvo sempre i metadati
        char *json = crea_json_evento(&evt);
        if (json) {
            sd_card_save_metadata(evt.id, json);
            free(json);
        }
        evt.saved_local = true;
        ESP_LOGI(TAG, "[SD] Salvato su SD");
    } else {
        ESP_LOGW(TAG, "[SD] SD card non disponibile");
        
        // FALLBACK RAM: se non c'è ne WiFi ne SD, salvo in RAM
        if (!upload_riuscito) {
            ESP_LOGE(TAG, "!!! Nessun WiFi e nessuna SD !!!");
            ESP_LOGW(TAG, "[RAM] Salvo in buffer RAM (ultimi %d eventi)", RAM_BUFFER_SIZE);
            salva_in_ram(&evt);
            led_set_pattern(LED_ERROR);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // --- FASE 6: cooldown ---
    stato_corrente = EVT_STATE_COOLDOWN;
    led_set_pattern(LED_IDLE);

    libera_frames(&ultimo_evento);
    memcpy(&ultimo_evento, &evt, sizeof(security_event_t));
    memset(evt.frames, 0, sizeof(evt.frames));

    // leggo cooldown effettivo da NVS
    uint32_t actual_cooldown = PIR_COOLDOWN_MS;
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            nvs_get_u32(nvs, NVS_KEY_COOLDOWN, &actual_cooldown);
            nvs_close(nvs);
        }
    }

    ESP_LOGI(TAG, "[COOLDOWN] Evento #%lu completato. Attesa %lu secondi...",
             (unsigned long)ultimo_evento.id, (unsigned long)(actual_cooldown / 1000));

    vTaskDelay(pdMS_TO_TICKS(actual_cooldown));
    stato_corrente = EVT_STATE_IDLE;
    ESP_LOGI(TAG, "[IDLE] Pronto per il prossimo evento\n");
}

// task che processa gli eventi dalla coda
static void event_task(void *arg)
{
    uint32_t trigger;
    while (1) {
        if (xQueueReceive(coda_eventi, &trigger, portMAX_DELAY)) {
            if (stato_corrente != EVT_STATE_IDLE) {
                ESP_LOGW(TAG, "Occupato (stato=%d), trigger ignorato", stato_corrente);
                continue;
            }
            stato_corrente = EVT_STATE_TRIGGERED;
            elabora_evento();
        }
    }
}

/*
 * FALLBACK PIR: se il PIR è scollegato, questo task fa "polling mode":
 * cattura un frame ogni POLLING_INTERVAL_MS e lo salva come evento.
 * È meno efficiente del PIR ma almeno il sistema fa qualcosa.
 */
#define POLLING_INTERVAL_MS 30000  // 30 secondi

static void polling_mode_task(void *arg)
{
    ESP_LOGW(TAG, "[POLLING] Modalita' polling attiva (ogni %ds)", POLLING_INTERVAL_MS / 1000);
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLLING_INTERVAL_MS));
        
        // controllo se il PIR è tornato attivo
        system_status_t st = error_monitor_get_status();
        if (st.pir_ok) {
            ESP_LOGI(TAG, "[POLLING] PIR ricollegato, esco dal polling mode");
            polling_task = NULL;
            vTaskDelete(NULL);  // mi auto-elimino
            return;
        }
        
        ESP_LOGW(TAG, "[POLLING] Cattura periodica (PIR assente)...");
        event_manager_motion_detected();  // simulo un trigger
    }
}

void event_manager_start_polling_mode(void)
{
    if (polling_task != NULL) return;  // già attivo
    
    xTaskCreate(polling_mode_task, "polling", 3072, NULL, 4, &polling_task);
}

void event_manager_stop_polling_mode(void)
{
    if (polling_task) {
        vTaskDelete(polling_task);
        polling_task = NULL;
        ESP_LOGI(TAG, "[POLLING] Modalita' polling disattivata");
    }
}

esp_err_t event_manager_init(void)
{
    coda_eventi = xQueueCreate(MAX_PENDING_EVENTS, sizeof(uint32_t));
    if (!coda_eventi) {
        ESP_LOGE(TAG, "Errore creazione coda eventi");
        return ESP_ERR_NO_MEM;
    }

    memset(ram_buffer, 0, sizeof(ram_buffer));

    // recupero il contatore eventi da NVS per non sovrascrivere
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_get_u32(nvs, "evt_counter", &contatore_eventi);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "Contatore eventi ripreso da NVS: %lu", (unsigned long)contatore_eventi);
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
         event_task, "event_task", 16384, NULL, 6, &task_handle, 1
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Errore creazione task eventi");
        return ESP_FAIL;
    }

    stato_corrente = EVT_STATE_IDLE;
    ESP_LOGI(TAG, "Event manager inizializzato");
    return ESP_OK;
}

void event_manager_motion_detected(void)
{
    uint32_t trigger = 1;
    if (coda_eventi) {
        xQueueSend(coda_eventi, &trigger, 0);
    }
}

event_state_t event_manager_get_state(void)
{
    return stato_corrente;
}

uint32_t event_manager_get_event_count(void)
{
    return contatore_eventi;
}

const security_event_t *event_manager_get_last_event(void)
{
    if (contatore_eventi > 0) return &ultimo_evento;
    return NULL;
}