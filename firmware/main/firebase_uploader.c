/*
 * firebase_uploader.c
 * ============================================================
 * Modulo per la comunicazione con Firebase Firestore tramite REST API.
 *
 *
 * Cosa fa:
 *  - Carica la configurazione Firebase da NVS (project_id, api_key, ...)
 *  - Si autentica anonimamente con Firebase Auth e ottiene un JWT
 *  - Rinnova automaticamente il token prima della scadenza (1 ora)
 *  - Carica i frame JPEG come documenti Firestore (codifica base64)
 *  - Carica i metadati degli eventi
 *  - Carica lo status del dispositivo ogni 30 secondi
 *  - Legge la configurazione remota lasciata dall'app
 *
 * ============================================================
 */
#include "firebase_uploader.h"
#include "board_config.h"
#include "event_manager.h"     // per attendere EVT_STATE_IDLE prima del self-deauth
#include "esp_http_client.h"   // client HTTP di ESP-IDF (equivalente OkHttp)
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"    // bundle CA root per verifica HTTPS
#include "esp_timer.h"         // per orario corrente in microsecondi
#include "esp_system.h"        // per esp_restart
#include "nvs.h"               // configurazione persistente (nvs_open, nvs_get/set_str)
#include "nvs_flash.h"         // per nvs_flash_erase() usato nel factory reset remoto
#include "cJSON.h"             // parsing JSON
#include "mbedtls/base64.h"    // codifica base64 dei JPEG
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"     // per vTaskDelay
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "FIREBASE";

// === CONFIGURAZIONE ===
// Caricata da NVS al boot
static char project_id[64] = {0};   // es: "security-cam-6a298"
static char api_key[64]    = {0};   // Web API Key di Firebase
static char bucket[128]    = {0};   // Storage bucket (non usato attivamente)
static bool configurato    = false; // true se project_id e api_key sono presenti

// === MULTI-DISPOSITIVO ===
// owner_uid e device_id sono opzionali, impostati dall'app via POST /config
// quando l'utente aggiunge il dispositivo. Se vuoti, uso path "flat".
// Se presenti, uso path nestato users/{uid}/devices/{id}/...
static char owner_uid[128] = {0};
static char device_id[64]  = {0};

// === TOKEN DI AUTENTICAZIONE ===
// Allocati dinamicamente con strdup() perche' la lunghezza varia.
static char *id_token = NULL;         // JWT per le richieste (scade dopo 1h)
static char *refresh_token = NULL;    // per rinnovare l'id_token
static int64_t token_expiry_ms = 0;   // timestamp di scadenza in millisecondi
static bool autenticato = false;

// === BUFFER PER RICEVERE LE RISPOSTE HTTP ===
// esp_http_client non ha "ricevi tutto in un buffer", devo usare un
// event handler che accumula i byte in questo buffer.
#define RESP_BUF_SIZE 2048
static char *resp_buffer = NULL;
static int resp_offset = 0;

// === FREEZE UPLOAD ===
// Quando true, TUTTI gli upload Firebase vengono rifiutati subito
// (ritornano ESP_ERR_INVALID_STATE senza fare alcuna chiamata HTTP).
//
// Viene attivato dal PRIMO "missing" rilevato da firebase_check_ownership()
// (sia 404 sia phantom document senza addedAt). Cosi' anche se aspetto le
// 3 conferme prima di self-deauth (per evitare false positive da rete),
// nel frattempo blocco subito eventuali eventi in corso o futuri dal
// ricreare phantom documents su Firestore.
//
// Una volta attivato, l'unico modo per "scongelarlo" e' un reboot
// (che lo riporta a false). Quindi il flusso normale e':
//   1. ownership check rileva 404/phantom -> upload_frozen = true
//   2. event_manager continua a lavorare ma upload diventano no-op
//   3. dopo 3 conferme -> self_deauth -> reboot -> upload_frozen = false
//      (ma a quel punto owner_uid/device_id sono vuoti, quindi gli upload
//       falliscono comunque per ESP_ERR_INVALID_STATE finche' l'utente
//       non riassocia il device dall'app)
static bool upload_frozen = false;


/*
 * carica_config()
 * --------------------------------------------------------------
 * Legge la configurazione Firebase da NVS.
 * Se mancano project_id o api_key, ritorna errore.
 * owner_uid e device_id sono opzionali (possono mancare al primo boot).
 */
static esp_err_t carica_config(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    // === CAMPI OBBLIGATORI ===
    size_t len;

    len = sizeof(project_id);
    err = nvs_get_str(nvs, NVS_KEY_FB_PROJECT, project_id, &len);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    len = sizeof(api_key);
    err = nvs_get_str(nvs, NVS_KEY_FB_APIKEY, api_key, &len);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    // bucket e' opzionale (non lo usiamo attivamente)
    len = sizeof(bucket);
    nvs_get_str(nvs, NVS_KEY_FB_BUCKET, bucket, &len);

    // === CAMPI OPZIONALI (multi-dispositivo) ===
    // se non presenti, lasciamo le stringhe vuote
    len = sizeof(owner_uid);
    if (nvs_get_str(nvs, NVS_KEY_OWNER_UID, owner_uid, &len) != ESP_OK) {
        owner_uid[0] = '\0';
    }
    len = sizeof(device_id);
    if (nvs_get_str(nvs, NVS_KEY_DEVICE_ID, device_id, &len) != ESP_OK) {
        device_id[0] = '\0';
    }

    nvs_close(nvs);
    return ESP_OK;
}

/*
 * auth_http_handler()
 * --------------------------------------------------------------
 * Event handler per esp_http_client.
 * esp_http_client funziona a callback: chiama questa funzione per
 * ogni evento HTTP (header, dati, fine, errore).
 * Io gestisco solo HTTP_EVENT_ON_DATA: accumulo i byte ricevuti
 * nel buffer globale resp_buffer per parsarli dopo.
 */
static esp_err_t auth_http_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // controllo che ci sia spazio nel buffer (per evitare overflow)
            if (resp_buffer && resp_offset + evt->data_len < RESP_BUF_SIZE - 1) {
                memcpy(resp_buffer + resp_offset, evt->data, evt->data_len);
                resp_offset += evt->data_len;
                resp_buffer[resp_offset] = '\0';  // null terminator
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/*
 * http_evt_handler()
 * --------------------------------------------------------------
 * Event handler "minimo" per le richieste in cui non mi serve
 * leggere la risposta (PATCH, POST con risposta ignorata).
 */
static esp_err_t http_evt_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ERROR) {
        ESP_LOGD(TAG, "Errore HTTP");
    }
    return ESP_OK;
}

/*
 * firebase_anonymous_login()
 * --------------------------------------------------------------
 * Fa il login anonimo via Firebase Auth REST API.
 * POST https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=API_KEY
 * Body: {"returnSecureToken":true}
 *
 * La risposta contiene:
 *   - idToken (JWT) valido 1 ora
 *   - refreshToken per rinnovare
 *   - expiresIn (secondi)
 */
static esp_err_t firebase_anonymous_login(void)
{
    ESP_LOGI(TAG, "Login anonimo Firebase...");

    // costruisco l'URL con la API key come query param
    char url[256];
    snprintf(url, sizeof(url),
        "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=%s",
        api_key);

    // body JSON: chiedo che la risposta contenga il token
    const char *body = "{\"returnSecureToken\":true}";

    // alloco il buffer per ricevere la risposta
    resp_buffer = (char *)calloc(1, RESP_BUF_SIZE);
    if (!resp_buffer) return ESP_ERR_NO_MEM;
    resp_offset = 0;

    // === CONFIGURAZIONE DEL CLIENT HTTP ===
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = auth_http_handler,       // callback per ricevere i dati
        .crt_bundle_attach = esp_crt_bundle_attach, // verifica CA per HTTPS
        .timeout_ms = 15000,                       // 15 secondi di timeout
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(resp_buffer); resp_buffer = NULL; return ESP_FAIL; }

    // imposto header e body
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    // esegui la richiesta (bloccante, ritorna quando finita)
    esp_err_t err = esp_http_client_perform(client);
    vTaskDelay(pdMS_TO_TICKS(200));
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    // verifico l'esito
    if (err != ESP_OK || http_code != 200) {
        ESP_LOGE(TAG, "Login anonimo fallito: HTTP %d", http_code);
        free(resp_buffer); resp_buffer = NULL;
        return ESP_FAIL;
    }

    // === PARSING DELLA RISPOSTA JSON ===
    cJSON *root = cJSON_Parse(resp_buffer);
    free(resp_buffer); resp_buffer = NULL;  // libero subito il buffer

    if (!root) {
        ESP_LOGE(TAG, "Errore parsing risposta login");
        return ESP_FAIL;
    }

    // estraggo i campi che mi servono
    cJSON *j_id_token = cJSON_GetObjectItem(root, "idToken");
    cJSON *j_refresh = cJSON_GetObjectItem(root, "refreshToken");
    cJSON *j_expires = cJSON_GetObjectItem(root, "expiresIn");

    if (!j_id_token || !j_id_token->valuestring ||
        !j_refresh || !j_refresh->valuestring) {
        ESP_LOGE(TAG, "Risposta login incompleta");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    // libero i token vecchi se presenti (non sarebbe la prima volta)
    if (id_token) free(id_token);
    if (refresh_token) free(refresh_token);

    // strdup = malloc + strcpy in una sola chiamata
    id_token = strdup(j_id_token->valuestring);
    refresh_token = strdup(j_refresh->valuestring);

    // calcolo la scadenza con 5 minuti di anticipo
    // cosi' rinnovo prima che sia effettivamente scaduto
    int expires_sec = 3600;  // default 1 ora
    if (j_expires && j_expires->valuestring) {
        expires_sec = atoi(j_expires->valuestring);
    }
    token_expiry_ms = (esp_timer_get_time() / 1000) + (expires_sec - 300) * 1000LL;

    autenticato = true;
    ESP_LOGI(TAG, "Login anonimo OK (token valido %d secondi)", expires_sec);

    cJSON_Delete(root);
    return ESP_OK;
}

/*
 * firebase_refresh_auth()
 * --------------------------------------------------------------
 * Rinnova il token usando il refreshToken.
 * POST https://securetoken.googleapis.com/v1/token?key=API_KEY
 * Body form-encoded: grant_type=refresh_token&refresh_token=REFRESH_TOKEN
 *
 * Notare che l'endpoint e' DIVERSO da signUp e usa nomi di campo
 * diversi nella risposta (id_token invece di idToken, snake_case
 * invece di camelCase). Cosi' ha deciso Google.
 */
static esp_err_t firebase_refresh_auth(void)
{
    ESP_LOGI(TAG, "Rinnovo token Firebase...");

    // se non ho il refresh token, faccio direttamente un nuovo login
    if (!refresh_token) return firebase_anonymous_login();

    char url[256];
    snprintf(url, sizeof(url),
        "https://securetoken.googleapis.com/v1/token?key=%s", api_key);

    // body in formato form-encoded (non JSON!)
    char *body = (char *)malloc(strlen(refresh_token) + 64);
    if (!body) return ESP_ERR_NO_MEM;
    snprintf(body, strlen(refresh_token) + 64,
        "grant_type=refresh_token&refresh_token=%s", refresh_token);

    resp_buffer = (char *)calloc(1, RESP_BUF_SIZE);
    if (!resp_buffer) { free(body); return ESP_ERR_NO_MEM; }
    resp_offset = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = auth_http_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body); free(resp_buffer); resp_buffer = NULL; return ESP_FAIL;
    }

    // Content-Type form-encoded (non application/json!)
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    vTaskDelay(pdMS_TO_TICKS(200));
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    // se il refresh fallisce, fallback su nuovo login
    if (err != ESP_OK || http_code != 200) {
        ESP_LOGE(TAG, "Refresh token fallito: HTTP %d, riprovo login", http_code);
        free(resp_buffer); resp_buffer = NULL;
        return firebase_anonymous_login();
    }

    cJSON *root = cJSON_Parse(resp_buffer);
    free(resp_buffer); resp_buffer = NULL;

    if (!root) return firebase_anonymous_login();

    // ATTENZIONE: nomi diversi rispetto a signUp!
    // signUp: idToken, refreshToken, expiresIn
    // refresh: id_token, refresh_token, expires_in
    cJSON *j_id  = cJSON_GetObjectItem(root, "id_token");
    cJSON *j_ref = cJSON_GetObjectItem(root, "refresh_token");
    cJSON *j_exp = cJSON_GetObjectItem(root, "expires_in");

    if (!j_id || !j_id->valuestring) {
        cJSON_Delete(root);
        return firebase_anonymous_login();
    }

    if (id_token) free(id_token);
    id_token = strdup(j_id->valuestring);

    if (j_ref && j_ref->valuestring) {
        if (refresh_token) free(refresh_token);
        refresh_token = strdup(j_ref->valuestring);
    }

    int expires_sec = 3600;
    if (j_exp && j_exp->valuestring) {
        expires_sec = atoi(j_exp->valuestring);
    }
    token_expiry_ms = (esp_timer_get_time() / 1000) + (expires_sec - 300) * 1000LL;

    ESP_LOGI(TAG, "Token rinnovato OK");
    cJSON_Delete(root);
    return ESP_OK;
}

/*
 * ensure_auth()
 * --------------------------------------------------------------
 * Garantisce che il token sia valido. Chiamata all'inizio di ogni
 * operazione Firebase.
 *  - Se non sono autenticato -> login
 *  - Se il token sta per scadere -> refresh
 *  - Altrimenti -> non fa nulla (e' veloce)
 */
static esp_err_t ensure_auth(void)
{
    if (!autenticato) {
        return firebase_anonymous_login();
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms >= token_expiry_ms) {
        ESP_LOGW(TAG, "Token scaduto, rinnovo...");
        return firebase_refresh_auth();
    }

    return ESP_OK;
}

/*
 * encode_base64()
 * --------------------------------------------------------------
 * Codifica un buffer di byte in stringa base64 ASCII.
 * Pattern in 2 chiamate (come nvs_get_blob):
 *  1. Prima chiamata con buffer NULL per ottenere la dimensione necessaria
 *  2. Allocazione del buffer
 *  3. Seconda chiamata per la codifica vera
 * Il chiamante deve fare free() del buffer ritornato.
 */
static char *encode_base64(const uint8_t *data, size_t data_len)
{
    // 1) calcola la dimensione necessaria
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, data, data_len);
    if (b64_len == 0) return NULL;

    // 2) alloca il buffer
    char *b64_buf = (char *)malloc(b64_len + 1);  // +1 per null terminator
    if (!b64_buf) return NULL;

    // 3) codifica vera
    int ret = mbedtls_base64_encode((unsigned char *)b64_buf, b64_len + 1,
                                     &b64_len, data, data_len);
    if (ret != 0) { free(b64_buf); return NULL; }
    b64_buf[b64_len] = '\0';
    return b64_buf;
}

/*
 * firebase_init()
 * --------------------------------------------------------------
 * Inizializza il modulo: carica la configurazione e fa il login.
 * Chiamata da main.c al passo [7].
 */
esp_err_t firebase_init(void)
{
    esp_err_t err = carica_config();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Config Firebase non trovata in NVS");
        return ESP_ERR_NOT_FOUND;
    }
    configurato = true;

    // log "decorativo" che mostra owner/device se configurati
    if (owner_uid[0] && device_id[0]) {
        ESP_LOGI(TAG, "Firebase OK: project='%s' owner='%.8s...' device='%s'",
                 project_id, owner_uid, device_id);
    } else {
        ESP_LOGI(TAG, "Firebase OK: project='%s' (no owner/device configurato)", project_id);
    }

    // tento il login subito (richiede WiFi gia' connesso)
    firebase_anonymous_login();

    return ESP_OK;
}

/*
 * firebase_upload_frame()
 * --------------------------------------------------------------
 * Carica un singolo frame JPEG come documento Firestore.
 * Path: events/evt_NNNNNN/frames/frame_M (o multi-dispositivo).
 *
 * Flusso:
 *  1. ensure_auth() per garantire token valido
 *  2. encode_base64() del JPEG
 *  3. Costruisce URL Firestore
 *  4. Costruisce JSON con il formato Firestore (fields + tipi)
 *  5. PATCH HTTP con header Authorization Bearer
 *  6. Gestisce codice di risposta
 */
esp_err_t firebase_upload_frame(const uint8_t *data, size_t length,
                                uint32_t event_id, int frame_idx)
{
    if (!configurato || !data) return ESP_ERR_INVALID_STATE;
    // Senza owner_uid/device_id finirei nel ramo "path flat" qui sotto
    // e scriverei alla root del database (events/... invece di
    // users/{uid}/devices/{id}/events/...). Dopo un self-deauth questo
    // creerebbe garbage che l'app non vede mai. Skip esplicito.
    if (!owner_uid[0] || !device_id[0]) {
        ESP_LOGW(TAG, "Upload frame skip: device non associato a un utente");
        return ESP_ERR_INVALID_STATE;
    }
    if (upload_frozen) return ESP_ERR_INVALID_STATE;
    // 1) garantisco token valido
    esp_err_t auth_err = ensure_auth();
    if (auth_err != ESP_OK) {
        ESP_LOGW(TAG, "Auth non disponibile, provo upload senza token...");
    }

    ESP_LOGI(TAG, "Upload frame %d (%u bytes) evento %lu...",
             frame_idx, (unsigned)length, (unsigned long)event_id);

    // 2) codifica base64 (Firestore non supporta dati binari)
    char *b64 = encode_base64(data, length);
    if (!b64) {
        ESP_LOGE(TAG, "Codifica base64 fallita");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Base64: %u -> %u bytes", (unsigned)length, (unsigned)strlen(b64));

    // 3) costruzione URL Firestore
    // %06lu = formatta l'event_id con zero-padding a 6 cifre
    // (es: evt_000042 invece di evt_42, per ordinamento corretto)
    char url[512];
    if (owner_uid[0] && device_id[0]) {
        // path NESTATO multi-dispositivo
        snprintf(url, sizeof(url),
            "https://firestore.googleapis.com/v1/projects/%s/"
            "databases/(default)/documents/"
            "users/%s/devices/%s/events/evt_%06lu/frames/frame_%d?key=%s",
            project_id, owner_uid, device_id,
            (unsigned long)event_id, frame_idx, api_key);
    } else {
        // path FLAT (compatibilita' senza multi-device)
        snprintf(url, sizeof(url),
            "https://firestore.googleapis.com/v1/projects/%s/"
            "databases/(default)/documents/"
            "events/evt_%06lu/frames/frame_%d?key=%s",
            project_id, (unsigned long)event_id, frame_idx, api_key);
    }

    // 4) costruzione JSON nel formato Firestore
    // Firestore richiede di wrappare ogni valore con il TIPO esplicito:
    //   { "fields": { "nome": { "stringValue": "valore" }, ... } }
    cJSON *root = cJSON_CreateObject();
    cJSON *fields = cJSON_AddObjectToObject(root, "fields");

    // image_base64: la stringa base64 del JPEG
    cJSON *f_data = cJSON_AddObjectToObject(fields, "image_base64");
    cJSON_AddStringToObject(f_data, "stringValue", b64);

    // jpeg_size: dimensione originale in byte
    // ATTENZIONE: integerValue accetta una STRINGA, non un numero!
    // (perche' i big int non sono rappresentabili in JSON standard)
    cJSON *f_size = cJSON_AddObjectToObject(fields, "jpeg_size");
    char size_str[16];
    snprintf(size_str, sizeof(size_str), "%u", (unsigned)length);
    cJSON_AddStringToObject(f_size, "integerValue", size_str);

    // frame_index: indice del frame nel burst (0, 1, 2, ...)
    cJSON *f_idx = cJSON_AddObjectToObject(fields, "frame_index");
    char idx_str[8];
    snprintf(idx_str, sizeof(idx_str), "%d", frame_idx);
    cJSON_AddStringToObject(f_idx, "integerValue", idx_str);

    // serializza il JSON in stringa
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(b64);  // libero il base64 dopo averlo copiato nel JSON

    if (!json) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Documento Firestore: %u bytes", (unsigned)strlen(json));

    // 5) configurazione client HTTP
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_PATCH,  // PATCH = crea o aggiorna documento
                                       // POST avrebbe creato un ID auto-generato
        .event_handler = http_evt_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,           // 30s, perche' i frame sono grandi
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(json); return ESP_FAIL; }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    // 6) header di autenticazione: "Authorization: Bearer <id_token>"
    if (id_token) {
        char *auth_header = (char *)malloc(strlen(id_token) + 16);
        if (auth_header) {
            snprintf(auth_header, strlen(id_token) + 16, "Bearer %s", id_token);
            esp_http_client_set_header(client, "Authorization", auth_header);
            free(auth_header);  // esp_http_client copia il valore internamente
        }
    }

    esp_http_client_set_post_field(client, json, strlen(json));

    // esegui la richiesta
    esp_err_t err = esp_http_client_perform(client);
    vTaskDelay(pdMS_TO_TICKS(200));
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(json);

    // === GESTIONE RISULTATO ===
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Upload fallito: %s", esp_err_to_name(err));
        return err;
    }
    if (http_code == 401 || http_code == 403) {
        // token rifiutato: forzo il rinnovo per il prossimo upload
        ESP_LOGW(TAG, "Token rifiutato (HTTP %d), rinnovo e riprovo", http_code);
        firebase_refresh_auth();
        return ESP_FAIL;  // non riprovo subito, sara' al prossimo evento
    }
    if (http_code < 200 || http_code >= 300) {
        ESP_LOGE(TAG, "Errore HTTP %d", http_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Frame %d salvato su Firestore OK", frame_idx);
    return ESP_OK;
}

/*
 * firebase_write_event()
 * --------------------------------------------------------------
 * Scrive il documento principale dell'evento (metadati).
 * Path: events/evt_NNNNNN
 * Viene chiamata DOPO firebase_upload_frame() per consolidare l'evento.
 *
 * Struttura simile a upload_frame ma piu' semplice (solo metadati).
 */
esp_err_t firebase_write_event(uint32_t event_id, int64_t timestamp,
                               int frame_count, int pir_value)
{
    if (!configurato) return ESP_ERR_INVALID_STATE;
    // Stesso ragionamento di firebase_upload_frame: senza owner/device
    // cadrei nel path flat che non e' nel namespace dell'utente.
    if (!owner_uid[0] || !device_id[0]) {
        ESP_LOGW(TAG, "Write event skip: device non associato a un utente");
        return ESP_ERR_INVALID_STATE;
    }
    if (upload_frozen) return ESP_ERR_INVALID_STATE;
    ensure_auth();

    // costruzione URL (nestato o flat)
    char url[512];
    if (owner_uid[0] && device_id[0]) {
        snprintf(url, sizeof(url),
            "https://firestore.googleapis.com/v1/projects/%s/"
            "databases/(default)/documents/"
            "users/%s/devices/%s/events/evt_%06lu?key=%s",
            project_id, owner_uid, device_id,
            (unsigned long)event_id, api_key);
    } else {
        snprintf(url, sizeof(url),
            "https://firestore.googleapis.com/v1/projects/%s/"
            "databases/(default)/documents/"
            "events/evt_%06lu?key=%s",
            project_id, (unsigned long)event_id, api_key);
    }

    // costruisco il JSON con i metadati dell'evento
    cJSON *root = cJSON_CreateObject();
    cJSON *fields = cJSON_AddObjectToObject(root, "fields");

    // event_id (intero)
    cJSON *f_id = cJSON_AddObjectToObject(fields, "event_id");
    char id_str[20];
    snprintf(id_str, sizeof(id_str), "%lu", (unsigned long)event_id);
    cJSON_AddStringToObject(f_id, "integerValue", id_str);

    // timestamp (intero a 64 bit)
    cJSON *f_ts = cJSON_AddObjectToObject(fields, "timestamp");
    char ts_str[24];
    snprintf(ts_str, sizeof(ts_str), "%lld", timestamp);
    cJSON_AddStringToObject(f_ts, "integerValue", ts_str);

    // frame_count (intero)
    cJSON *f_fc = cJSON_AddObjectToObject(fields, "frame_count");
    char fc_str[8];
    snprintf(fc_str, sizeof(fc_str), "%d", frame_count);
    cJSON_AddStringToObject(f_fc, "integerValue", fc_str);

    // pir_value (intero)
    cJSON *f_pir = cJSON_AddObjectToObject(fields, "pir_value");
    char pir_str[8];
    snprintf(pir_str, sizeof(pir_str), "%d", pir_value);
    cJSON_AddStringToObject(f_pir, "integerValue", pir_str);

    // validated (booleano) - inizialmente false, l'app lo settera' a true
    // dopo aver fatto l'analisi ML Kit
    cJSON *f_val = cJSON_AddObjectToObject(fields, "validated");
    cJSON_AddBoolToObject(f_val, "booleanValue", false);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    // configurazione e chiamata HTTP (uguale a upload_frame)
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_PATCH,
        .event_handler = http_evt_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(json); return ESP_FAIL; }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (id_token) {
        char *auth_header = (char *)malloc(strlen(id_token) + 16);
        if (auth_header) {
            snprintf(auth_header, strlen(id_token) + 16, "Bearer %s", id_token);
            esp_http_client_set_header(client, "Authorization", auth_header);
            free(auth_header);
        }
    }

    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    vTaskDelay(pdMS_TO_TICKS(200));
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(json);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scrittura evento fallita: %s", esp_err_to_name(err));
        return err;
    }
    if (http_code == 401 || http_code == 403) {
        ESP_LOGW(TAG, "Token rifiutato, rinnovo al prossimo evento");
        firebase_refresh_auth();
        return ESP_FAIL;
    }
    if (http_code < 200 || http_code >= 300) {
        ESP_LOGE(TAG, "Firestore errore HTTP %d", http_code);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Evento %lu scritto su Firestore", (unsigned long)event_id);
    return ESP_OK;
}

/*
 * firebase_is_ready()
 * --------------------------------------------------------------
 * Getter pubblico: ritorna true se il modulo e' configurato.
 * NON verifica la connessione internet (per quello c'e' wifi_manager_is_connected).
 */
bool firebase_is_ready(void)
{
    return configurato;
}

/*
 * firebase_upload_status()
 * --------------------------------------------------------------
 * Carica lo stato corrente del dispositivo su Firestore.
 * Path: users/{uid}/devices/{id}/status/current
 *
 * Chiamata dall'error_monitor ogni 30 secondi.
 * L'app Android legge questo documento per la dashboard remota.
 *
 * Funziona SOLO se owner_uid e device_id sono impostati.
 */
esp_err_t firebase_upload_status(bool camera_ok, bool pir_ok, bool sd_ok,
                                  bool wifi_ok, bool fb_ok,
                                  uint32_t uptime_s, uint32_t free_heap,
                                  uint32_t total_events, uint32_t failed_uploads,
                                  const char *state)
{
    if (!configurato || !owner_uid[0] || !device_id[0]) return ESP_ERR_INVALID_STATE;
    if (upload_frozen) return ESP_ERR_INVALID_STATE;
    esp_err_t auth_err = ensure_auth();
    if (auth_err != ESP_OK) return auth_err;

    // URL: documento status/current (nome fisso, non timestamp)
    // Cosi' ogni upload sovrascrive il precedente
    char url[512];
    snprintf(url, sizeof(url),
        "https://firestore.googleapis.com/v1/projects/%s/"
        "databases/(default)/documents/"
        "users/%s/devices/%s/status/current?key=%s",
        project_id, owner_uid, device_id, api_key);

    // costruisco il JSON con TUTTI i campi dello status
    cJSON *root = cJSON_CreateObject();
    cJSON *fields = cJSON_AddObjectToObject(root, "fields");

    // === BOOLEANI (stato periferiche) ===
    cJSON *fc = cJSON_AddObjectToObject(fields, "camera_ok");
    cJSON_AddBoolToObject(fc, "booleanValue", camera_ok);
    cJSON *fp = cJSON_AddObjectToObject(fields, "pir_ok");
    cJSON_AddBoolToObject(fp, "booleanValue", pir_ok);
    cJSON *fs = cJSON_AddObjectToObject(fields, "sd_card_ok");
    cJSON_AddBoolToObject(fs, "booleanValue", sd_ok);
    cJSON *fw = cJSON_AddObjectToObject(fields, "wifi_ok");
    cJSON_AddBoolToObject(fw, "booleanValue", wifi_ok);
    cJSON *ff = cJSON_AddObjectToObject(fields, "firebase_ok");
    cJSON_AddBoolToObject(ff, "booleanValue", fb_ok);

    // === INTERI (statistiche) ===
    char buf[20];
    cJSON *fu = cJSON_AddObjectToObject(fields, "uptime_s");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)uptime_s);
    cJSON_AddStringToObject(fu, "integerValue", buf);

    cJSON *fh = cJSON_AddObjectToObject(fields, "free_heap");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)free_heap);
    cJSON_AddStringToObject(fh, "integerValue", buf);

    cJSON *fe = cJSON_AddObjectToObject(fields, "total_events");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)total_events);
    cJSON_AddStringToObject(fe, "integerValue", buf);

    cJSON *ffu = cJSON_AddObjectToObject(fields, "failed_uploads");
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)failed_uploads);
    cJSON_AddStringToObject(ffu, "integerValue", buf);

    // === STRINGHE (state della pipeline) ===
    cJSON *fst = cJSON_AddObjectToObject(fields, "state");
    cJSON_AddStringToObject(fst, "stringValue", state ? state : "unknown");

    // === ORARIO CORRENTE ===
    // ottengo l'orario locale e lo formatto in stringa leggibile
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    cJSON *ftm = cJSON_AddObjectToObject(fields, "time");
    cJSON_AddStringToObject(ftm, "stringValue", time_buf);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    // chiamata HTTP (PATCH come per gli eventi)
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_PATCH,
        .event_handler = http_evt_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(json); return ESP_FAIL; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (id_token) {
        char *auth_header = (char *)malloc(strlen(id_token) + 16);
        if (auth_header) {
            snprintf(auth_header, strlen(id_token) + 16, "Bearer %s", id_token);
            esp_http_client_set_header(client, "Authorization", auth_header);
            free(auth_header);
        }
    }
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    vTaskDelay(pdMS_TO_TICKS(200));
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(json);

    if (err != ESP_OK || http_code < 200 || http_code >= 300) {
        ESP_LOGW(TAG, "Upload status fallito: HTTP %d", http_code);
        return ESP_FAIL;
    }

    return ESP_OK;
}


/*
 * firebase_check_ownership()
 * --------------------------------------------------------------
 * Verifica se questo dispositivo e' ancora "di proprieta'"
 * dell'utente registrato in NVS.
 *
 * Fa una GET su:  users/{owner_uid}/devices/{device_id}
 *
 * Casi possibili:
 *  - HTTP 200 + documento ha il campo "addedAt"
 *      -> il device esiste regolarmente -> ESP_OK
 *  - HTTP 200 ma documento SENZA "addedAt" (phantom document)
 *      -> l'utente ha cancellato il device dall'app, ma sono
 *         rimaste sottocollezioni orfane che fanno apparire
 *         il documento padre in console come "in italico"
 *      -> ESP_ERR_NOT_FOUND
 *  - HTTP 404
 *      -> il documento non esiste proprio -> ESP_ERR_NOT_FOUND
 *  - errori di rete, timeout, 5xx, problemi token
 *      -> NON e' una conferma che il device sia stato cancellato
 *      -> ESP_FAIL  (il chiamante NON deve trattarlo come "missing")
 *
 * Questa distinzione tra "sicuramente cancellato" e "incerto" e'
 * fondamentale per evitare false deauth (es. WiFi che balla e
 * ti deautentica per sbaglio).
 *
 * SIDE EFFECT: appena rileva un caso "missing" (404 oppure phantom),
 * setta upload_frozen=true. Cosi' tutti gli upload futuri sono
 * rifiutati subito senza fare HTTP, evitando di creare phantom
 * documents nei 90 secondi che separano il primo missing dal
 * self-deauth definitivo.
 */
esp_err_t firebase_check_ownership(void)
{
    if (!configurato || !owner_uid[0] || !device_id[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t auth_err = ensure_auth();
    if (auth_err != ESP_OK) return ESP_FAIL;

    char url[512];
    snprintf(url, sizeof(url),
        "https://firestore.googleapis.com/v1/projects/%s/"
        "databases/(default)/documents/"
        "users/%s/devices/%s?key=%s",
        project_id, owner_uid, device_id, api_key);

    resp_buffer = (char *)calloc(1, RESP_BUF_SIZE);
    if (!resp_buffer) return ESP_FAIL;
    resp_offset = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = auth_http_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(resp_buffer); resp_buffer = NULL;
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (id_token) {
        char *auth_header = (char *)malloc(strlen(id_token) + 16);
        if (auth_header) {
            snprintf(auth_header, strlen(id_token) + 16, "Bearer %s", id_token);
            esp_http_client_set_header(client, "Authorization", auth_header);
            free(auth_header);
        }
    }

    esp_err_t err = esp_http_client_perform(client);
    vTaskDelay(pdMS_TO_TICKS(200));
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Ownership check: HTTP %d, err=%s",
             http_code, esp_err_to_name(err));

    if (err != ESP_OK) {
        free(resp_buffer); resp_buffer = NULL;
        return ESP_FAIL;  // errore di rete: NON deauth
    }

    // 404 = il documento NON esiste = device cancellato
    if (http_code == 404) {
        free(resp_buffer); resp_buffer = NULL;
        ESP_LOGW(TAG, "Ownership: documento NON trovato (404)");
        if (!upload_frozen) {
            upload_frozen = true;
            ESP_LOGW(TAG, "Upload Firebase CONGELATI (in attesa di self-deauth)");
        }
        return ESP_ERR_NOT_FOUND;
    }

    // problema di autorizzazione: tentativo di refresh token al
    // prossimo giro, NON considero come "device cancellato"
    if (http_code == 401 || http_code == 403) {
        free(resp_buffer); resp_buffer = NULL;
        firebase_refresh_auth();
        return ESP_FAIL;
    }

    if (http_code != 200) {
        // 5xx, 0, ecc. -> errore transitorio, non deauth
        free(resp_buffer); resp_buffer = NULL;
        return ESP_FAIL;
    }

    // === HTTP 200: parso il JSON e cerco il campo "addedAt" ===
    // Se il documento e' un phantom (esiste solo perche' ci sono
    // sottocollezioni sotto), il body avra' la struttura senza il
    // campo "fields", oppure "fields" senza "addedAt".
    cJSON *root = cJSON_Parse(resp_buffer);
    free(resp_buffer); resp_buffer = NULL;
    if (!root) return ESP_FAIL;

    cJSON *fields = cJSON_GetObjectItem(root, "fields");
    bool has_added_at = false;
    if (fields) {
        cJSON *added = cJSON_GetObjectItem(fields, "addedAt");
        if (added) has_added_at = true;
    }
    cJSON_Delete(root);

    if (!has_added_at) {
        ESP_LOGW(TAG, "Ownership: documento phantom (no addedAt)");
        if (!upload_frozen) {
            upload_frozen = true;
            ESP_LOGW(TAG, "Upload Firebase CONGELATI (in attesa di self-deauth)");
        }
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

/*
 * firebase_self_deauth()
 * --------------------------------------------------------------
 * "Auto-deautorizzazione soft" del dispositivo:
 *  0. Aspetto che event_manager torni in IDLE (max 30s) cosi' non
 *     interrompo upload in corso a meta'. Anche se gli upload sono
 *     gia' "frozen" (no-op), il TLS/HTTP in volo deve completare
 *     pulitamente per non lasciare stato corrotto.
 *  1. Tento di cancellare le mie sottocollezioni residue su
 *     Firestore (status/current, pending_config/current) cosi'
 *     da non lasciare phantom documents anche dopo il deauth.
 *     Best-effort: se fallisce, va bene lo stesso.
 *  2. Cancello SOLO owner_uid e device_id da NVS.
 *     NON tocco WiFi ne' Firebase config: l'ESP al riavvio
 *     resta online sulla rete, raggiungibile via HTTP, e
 *     l'utente puo' riassociarlo dall'app col flusso normale.
 *  3. Riavvio.
 *
 * Non e' un factory reset completo: e' un "scollegati da
 * questo account, ma resta operativo a livello hardware".
 *
 * Differenza vs factory reset (handler /factory_reset):
 *  - factory_reset cancella TUTTO (WiFi, Firebase, certificati)
 *    e torna in modalita' SoftAP "SecurityCam-Setup"
 *  - self_deauth cancella SOLO il binding utente, mantiene
 *    la rete cosi' l'utente puo' riassociare via HTTP/app
 *    senza dover ricollegarsi al SoftAP
 */
esp_err_t firebase_self_deauth(void)
{
    ESP_LOGW(TAG, "=== SELF-DEAUTH: device cancellato dall'utente ===");

    // --- STEP 0: aspetto che event_manager torni in IDLE ---
    // Anche se gli upload sono frozen, un evento gia' partito potrebbe
    // ancora avere chiamate HTTP/TLS in volo. Aspetto al massimo 30
    // secondi (piu' che sufficiente per finire un upload medio); se
    // scade, procedo comunque perche' upload_frozen=true mi garantisce
    // che le scritture Firebase sono no-op.
    int waited_ms = 0;
    while (event_manager_get_state() != EVT_STATE_IDLE && waited_ms < 30000) {
        ESP_LOGI(TAG, "Self-deauth: attendo fine evento in corso (state=%d)...",
                 event_manager_get_state());
        vTaskDelay(pdMS_TO_TICKS(2000));
        waited_ms += 2000;
    }
    if (event_manager_get_state() != EVT_STATE_IDLE) {
        ESP_LOGW(TAG, "Self-deauth: timeout attesa idle, procedo comunque");
    } else {
        ESP_LOGI(TAG, "Self-deauth: event_manager in IDLE, procedo");
    }

    // --- STEP 1: tentativo di cleanup remoto delle mie sottocollezioni ---
    // Best-effort, ignoro errori. Se non riesco, rimangono dati orfani
    // ma il device principale comunque non riapparira' nell'app.
    if (configurato && owner_uid[0] && device_id[0] && id_token) {
        const char *paths[] = {
            "status/current",
            "pending_config/current",
            NULL
        };
        for (int i = 0; paths[i] != NULL; i++) {
            char url[512];
            snprintf(url, sizeof(url),
                "https://firestore.googleapis.com/v1/projects/%s/"
                "databases/(default)/documents/"
                "users/%s/devices/%s/%s?key=%s",
                project_id, owner_uid, device_id, paths[i], api_key);

            esp_http_client_config_t del_cfg = {
                .url = url,
                .method = HTTP_METHOD_DELETE,
                .event_handler = http_evt_handler,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms = 8000,
                // buffer ampi per contenere l'header Authorization Bearer
                // (i JWT di Firebase sono ~1KB, il default di 512 byte
                // produce il warning "Buffer length is small to fit all
                // the headers" e fa fallire la DELETE)
                .buffer_size = 2048,
                .buffer_size_tx = 2048,
            };
            esp_http_client_handle_t c = esp_http_client_init(&del_cfg);
            if (c) {
                char *h = (char *)malloc(strlen(id_token) + 16);
                if (h) {
                    snprintf(h, strlen(id_token) + 16, "Bearer %s", id_token);
                    esp_http_client_set_header(c, "Authorization", h);
                    free(h);
                }
                esp_http_client_perform(c);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_http_client_cleanup(c);
                ESP_LOGI(TAG, "Self-deauth: DELETE %s tentato", paths[i]);
            }
        }
    }

    // --- STEP 2: cancello owner_uid e device_id da NVS ---
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_OWNER_UID);
        nvs_erase_key(nvs, NVS_KEY_DEVICE_ID);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGW(TAG, "Self-deauth: NVS pulito (owner_uid + device_id)");
    }

    // svuoto anche le copie in RAM
    owner_uid[0] = '\0';
    device_id[0] = '\0';

    // --- STEP 3: riavvio ---
    ESP_LOGW(TAG, "Self-deauth: riavvio in corso. ESP restera' online "
                  "ma in attesa di riassociazione utente.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;  // mai raggiunto
}

/*
 * firebase_check_remote_config()
 * --------------------------------------------------------------
 * Controlla se l'app ha lasciato una configurazione in attesa
 * su Firestore (path: users/{uid}/devices/{id}/pending_config/current).
 * Se si', applica i nuovi valori in NVS, cancella il documento,
 * e riavvia l'ESP per applicarli.
 *
 * Chiamata dall'error_monitor ogni 30 secondi.
 *
 * E' il meccanismo di CONFIGURAZIONE REMOTA quando l'app non e'
 * sulla stessa rete dell'ESP (es. utente fuori casa).
 */
esp_err_t firebase_check_remote_config(void)
{
    if (!configurato || !owner_uid[0] || !device_id[0]) return ESP_ERR_INVALID_STATE;

    esp_err_t auth_err = ensure_auth();
    if (auth_err != ESP_OK) return auth_err;

    // === GET del documento pending_config ===
    char url[512];
    snprintf(url, sizeof(url),
        "https://firestore.googleapis.com/v1/projects/%s/"
        "databases/(default)/documents/"
        "users/%s/devices/%s/pending_config/current?key=%s",
        project_id, owner_uid, device_id, api_key);

    // alloco buffer per la risposta
    resp_buffer = (char *)calloc(1, RESP_BUF_SIZE);
    if (!resp_buffer) return ESP_ERR_NO_MEM;
    resp_offset = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,            // GET, non PATCH
        .event_handler = auth_http_handler,    // ho bisogno della risposta
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(resp_buffer); resp_buffer = NULL; return ESP_FAIL; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (id_token) {
        char *auth_header = (char *)malloc(strlen(id_token) + 16);
        if (auth_header) {
            snprintf(auth_header, strlen(id_token) + 16, "Bearer %s", id_token);
            esp_http_client_set_header(client, "Authorization", auth_header);
            free(auth_header);
        }
    }

    esp_err_t err = esp_http_client_perform(client);
    vTaskDelay(pdMS_TO_TICKS(200));
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Config remota GET: HTTP %d, err=%s", http_code, esp_err_to_name(err));

    if (err != ESP_OK) {
        free(resp_buffer); resp_buffer = NULL;
        return ESP_FAIL;
    }

    // 404 = il documento non esiste = nessuna config in attesa
    // 0 = nessuna risposta (puo' succedere)
    if (http_code == 404 || http_code == 0) {
        free(resp_buffer); resp_buffer = NULL;
        return ESP_OK;  // tutto ok, semplicemente non c'e' niente
    }
    if (http_code != 200) {
        ESP_LOGW(TAG, "Config remota risposta: %s", resp_buffer ? resp_buffer : "vuota");
        free(resp_buffer); resp_buffer = NULL;
        return ESP_FAIL;
    }

    // === PARSE DEL JSON DI RISPOSTA ===
    cJSON *root = cJSON_Parse(resp_buffer);
    free(resp_buffer); resp_buffer = NULL;
    if (!root) return ESP_FAIL;

    cJSON *fields = cJSON_GetObjectItem(root, "fields");
    if (!fields) { cJSON_Delete(root); return ESP_OK; }

    // === LETTURA E SALVATAGGIO IN NVS ===
    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) { cJSON_Delete(root); return err; }

    bool config_changed = false;

    // === FACTORY RESET REMOTO ===
    // L'app puo' inviare {factory_reset: true} per forzare un wipe
    // completo del dispositivo quando NON e' sulla stessa rete dell'ESP
    // (e quindi non puo' chiamare l'endpoint HTTP /factory_reset).
    //
    // Riusa il canale pending_config che gia' esiste per la config
    // remota di cooldown/frame_count/jpeg_quality.
    //
    // Differenza vs self_deauth:
    //  - self_deauth e' "soft": cancella solo owner_uid+device_id,
    //    mantiene WiFi e Firebase config, l'ESP resta sulla stessa
    //    rete e l'utente puo' riassociarlo dall'app.
    //  - factory_reset e' "hard": cancella TUTTO NVS (WiFi, Firebase,
    //    owner, device), l'ESP riavvia in modalita' SoftAP
    //    "SecurityCam-Setup" e l'utente deve rifare il provisioning
    //    da zero come al primo avvio.
    cJSON *fr = cJSON_GetObjectItem(fields, "factory_reset");
    if (fr) {
        cJSON *fr_val = cJSON_GetObjectItem(fr, "booleanValue");
        if (fr_val && cJSON_IsTrue(fr_val)) {
            ESP_LOGW(TAG, "*** FACTORY RESET REMOTO richiesto dall'app ***");

            // 1. Chiudo l'handle NVS aperto sopra (l'avevo aperto per
            //    eventuali update di cooldown/etc. che ora non faro' piu')
            nvs_close(nvs);
            cJSON_Delete(root);

            // 2. Cancello la pending_config su Firestore cosi' al
            //    riavvio non riapplico il comando in loop infinito.
            //    Best-effort: se fallisce non e' grave perche' dopo
            //    il wipe NVS l'ESP non legge piu' Firestore.
            esp_http_client_config_t del_cfg = {
                .url = url,
                .method = HTTP_METHOD_DELETE,
                .event_handler = http_evt_handler,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms = 8000,
                .buffer_size = 2048,
                .buffer_size_tx = 2048,
            };
            esp_http_client_handle_t dc = esp_http_client_init(&del_cfg);
            if (dc) {
                if (id_token) {
                    char *h = (char *)malloc(strlen(id_token) + 16);
                    if (h) {
                        snprintf(h, strlen(id_token) + 16, "Bearer %s", id_token);
                        esp_http_client_set_header(dc, "Authorization", h);
                        free(h);
                    }
                }
                esp_http_client_perform(dc);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_http_client_cleanup(dc);
                ESP_LOGI(TAG, "Factory reset remoto: pending_config cancellata");
            }

            // 3. Aspetto che event_manager torni in IDLE (max 30s) per
            //    non interrompere upload in corso. Stesso pattern di
            //    self_deauth.
            int waited_ms = 0;
            while (event_manager_get_state() != EVT_STATE_IDLE && waited_ms < 30000) {
                ESP_LOGI(TAG, "Factory reset remoto: attendo idle "
                              "(state=%d)...", event_manager_get_state());
                vTaskDelay(pdMS_TO_TICKS(2000));
                waited_ms += 2000;
            }

            // 4. Wipe COMPLETO di NVS: cancella WiFi, Firebase, owner,
            //    device, configurato, tutto. Al prossimo boot
            //    provisioning_is_configured() ritornera' false e
            //    l'ESP entrera' in modalita' SoftAP "SecurityCam-Setup".
            ESP_LOGW(TAG, "Factory reset remoto: cancello tutto NVS");
            nvs_flash_erase();

            // 5. Riavvio
            ESP_LOGW(TAG, "Factory reset remoto: riavvio in modalita' "
                          "configurazione");
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
            return ESP_OK;  // mai raggiunto
        }
    }

    // cooldown_ms
    cJSON *cooldown = cJSON_GetObjectItem(fields, "cooldown_ms");
    if (cooldown) {
        cJSON *val = cJSON_GetObjectItem(cooldown, "integerValue");
        if (val && val->valuestring) {
            uint32_t v = (uint32_t)atoi(val->valuestring);
            // valido il range (5-60 secondi)
            if (v >= 5000 && v <= 60000) {
                nvs_set_u32(nvs, NVS_KEY_COOLDOWN, v);
                ESP_LOGI(TAG, "Config remota: cooldown=%lu ms", (unsigned long)v);
                config_changed = true;
            }
        }
    }

    // frame_count
    cJSON *frames = cJSON_GetObjectItem(fields, "frame_count");
    if (frames) {
        cJSON *val = cJSON_GetObjectItem(frames, "integerValue");
        if (val && val->valuestring) {
            int v = atoi(val->valuestring);
            if (v >= 1 && v <= 5) {
                nvs_set_u8(nvs, NVS_KEY_FRAME_COUNT, (uint8_t)v);
                ESP_LOGI(TAG, "Config remota: frame_count=%d", v);
                config_changed = true;
            }
        }
    }

    // jpeg_quality
    cJSON *quality = cJSON_GetObjectItem(fields, "jpeg_quality");
    if (quality) {
        cJSON *val = cJSON_GetObjectItem(quality, "integerValue");
        if (val && val->valuestring) {
            int v = atoi(val->valuestring);
            if (v >= 5 && v <= 50) {
                nvs_set_u8(nvs, NVS_KEY_QUALITY, (uint8_t)v);
                ESP_LOGI(TAG, "Config remota: jpeg_quality=%d", v);
                config_changed = true;
            }
        }
    }

    nvs_commit(nvs);
    nvs_close(nvs);
    cJSON_Delete(root);

    // === SE QUALCOSA E' CAMBIATO: DELETE + RIAVVIO ===
    if (config_changed) {
        ESP_LOGW(TAG, "Config remota applicata! Cancello documento pendente...");

        // DELETE del documento per non riapplicarlo al prossimo check
        esp_http_client_config_t del_cfg = {
            .url = url,
            .method = HTTP_METHOD_DELETE,
            .event_handler = http_evt_handler,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
        };

        esp_http_client_handle_t del_client = esp_http_client_init(&del_cfg);
        if (del_client) {
            // anche per la DELETE serve l'header di autenticazione
            if (id_token) {
                char *auth_header = (char *)malloc(strlen(id_token) + 16);
                if (auth_header) {
                    snprintf(auth_header, strlen(id_token) + 16, "Bearer %s", id_token);
                    esp_http_client_set_header(del_client, "Authorization", auth_header);
                    free(auth_header);
                }
            }
            esp_http_client_perform(del_client);
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_http_client_cleanup(del_client);
        }

        // riavvio l'ESP per applicare la nuova configurazione
        // (i nuovi valori vengono letti da NVS al boot dei moduli)
        ESP_LOGW(TAG, "Riavvio per applicare la nuova configurazione...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    return ESP_OK;
}