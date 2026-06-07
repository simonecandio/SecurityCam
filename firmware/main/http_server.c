/*
 * http_server.c
 * Server HTTP con autenticazione Basic Auth su tutti gli endpoint.
 * 
 * Sicurezza implementata:
 * - HTTP Basic Auth (RFC 7617) su ogni endpoint
 * - Credenziali di default: admin/securitycam
 *   (in produzione andrebbero salvate in NVS e configurabili)
 * 
 * Quando il browser o l'app accede a qualsiasi endpoint,
 * il server risponde con 401 + WWW-Authenticate se mancano
 * le credenziali. Il browser mostra automaticamente il popup login.
 */
#include "http_server.h"
#include "camera_handler.h"
#include "event_manager.h"
#include "error_monitor.h"
#include "wifi_manager.h"
#include "board_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "cJSON.h"
#include "nvs.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <time.h>
#include "cert_manager.h"
#ifdef CONFIG_ESP_HTTPS_SERVER_ENABLE
#include "esp_https_server.h"
#endif

static const char *TAG = "HTTP";
static httpd_handle_t server = NULL;

// credenziali Basic Auth
#define HTTP_AUTH_USER "YOUR_AUTH_USER"
#define HTTP_AUTH_PASS "YOUR_AUTH_PASS"

#define BOUNDARY        "frame"
#define STREAM_TYPE     "multipart/x-mixed-replace;boundary=" BOUNDARY
#define PART_HEADER     "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

// ===== AUTENTICAZIONE =====
/*
 * Verifica HTTP Basic Auth (RFC 7617).
 * L'header Authorization contiene "Basic <base64(user:pass)>"
 * Se manca o è errato, manda 401 con WWW-Authenticate.
 * Restituisce true se autenticato, false altrimenti.
 */
static bool verifica_auth(httpd_req_t *req)
{
    char auth_buf[128] = {0};

    // leggo l'header Authorization
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", auth_buf, sizeof(auth_buf));
    if (err != ESP_OK) {
        // nessun header -> mando 401 con richiesta di autenticazione
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SecurityCam\"");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_sendstr(req, "Autenticazione richiesta");
        ESP_LOGW(TAG, "Accesso negato: nessuna credenziale");
        return false;
    }

    // verifico che sia Basic auth
    if (strncmp(auth_buf, "Basic ", 6) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SecurityCam\"");
        httpd_resp_sendstr(req, "Tipo autenticazione non supportato");
        return false;
    }

    // decodifico il base64 dopo "Basic "
    const char *b64_credentials = auth_buf + 6;
    unsigned char decoded[128] = {0};
    size_t decoded_len = 0;

    int ret = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                                     (const unsigned char *)b64_credentials,
                                     strlen(b64_credentials));
    if (ret != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SecurityCam\"");
        httpd_resp_sendstr(req, "Credenziali malformate");
        ESP_LOGW(TAG, "Accesso negato: base64 non valido");
        return false;
    }
    decoded[decoded_len] = '\0';

    // il formato decodificato è "utente:password"
    char atteso[128];
    snprintf(atteso, sizeof(atteso), "%s:%s", HTTP_AUTH_USER, HTTP_AUTH_PASS);

    if (strcmp((char *)decoded, atteso) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SecurityCam\"");
        httpd_resp_sendstr(req, "Credenziali errate");
        ESP_LOGW(TAG, "Accesso negato: credenziali errate");
        return false;
    }

    // autenticato!
    return true;
}

// ===== HANDLER: /stream (MJPEG) =====
static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!verifica_auth(req)) return ESP_OK;

    ESP_LOGI(TAG, "Stream: client autenticato connesso");

    httpd_resp_set_type(req, STREAM_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char hdr_buf[128];
    esp_err_t res = ESP_OK;

    while (res == ESP_OK) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Stream: cattura fallita");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int hdr_len = snprintf(hdr_buf, sizeof(hdr_buf), PART_HEADER, (unsigned)fb->len);
        res = httpd_resp_send_chunk(req, hdr_buf, hdr_len);
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(66));
    }

    ESP_LOGI(TAG, "Stream: client disconnesso");
    return res;
}

// ===== HANDLER: /capture (singolo JPEG) =====
static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!verifica_auth(req)) return ESP_OK;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cattura fallita");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

// ===== HANDLER: /status (JSON) =====
static esp_err_t status_handler(httpd_req_t *req)
{
    if (!verifica_auth(req)) return ESP_OK;

    system_status_t st = error_monitor_get_status();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "SecurityCam-ESP32S3");
    cJSON_AddStringToObject(root, "firmware", "1.0.0");
    cJSON_AddNumberToObject(root, "uptime_s", st.uptime_seconds);
    cJSON_AddNumberToObject(root, "free_heap", st.free_heap);
    cJSON_AddBoolToObject(root, "camera_ok", st.camera_ok);
    cJSON_AddBoolToObject(root, "pir_ok", st.pir_ok);
    cJSON_AddBoolToObject(root, "sd_card_ok", st.sd_card_ok);
    cJSON_AddBoolToObject(root, "wifi_ok", st.wifi_ok);
    cJSON_AddBoolToObject(root, "firebase_ok", st.firebase_ok);
    cJSON_AddNumberToObject(root, "total_events", st.total_events);
    cJSON_AddNumberToObject(root, "failed_uploads", st.failed_uploads);

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
    cJSON_AddStringToObject(root, "state", state_str);

    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(root, "ip", ip);

    // aggiungo orario corrente
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    cJSON_AddStringToObject(root, "time", time_buf);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return res;
}

// ===== HANDLER: POST /config (configurazione remota da app) =====
static esp_err_t config_handler(httpd_req_t *req)
{
    if (!verifica_auth(req)) return ESP_OK;

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body vuoto");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"JSON non valido\"}");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"NVS error\"}");
        return ESP_FAIL;
    }

    // cooldown
    cJSON *cooldown = cJSON_GetObjectItem(root, "cooldown_ms");
    if (cooldown && cJSON_IsNumber(cooldown)) {
        nvs_set_u32(nvs, NVS_KEY_COOLDOWN, (uint32_t)cooldown->valuedouble);
        ESP_LOGI(TAG, "Config: cooldown=%d ms", (int)cooldown->valuedouble);
    }

    // numero frame
    cJSON *frames = cJSON_GetObjectItem(root, "frame_count");
    if (frames && cJSON_IsNumber(frames)) {
        int val = (int)frames->valuedouble;
        if (val >= 1 && val <= 5) {
            nvs_set_u8(nvs, NVS_KEY_FRAME_COUNT, (uint8_t)val);
            ESP_LOGI(TAG, "Config: frame_count=%d", val);
        }
    }

    // qualità JPEG
    cJSON *quality = cJSON_GetObjectItem(root, "jpeg_quality");
    if (quality && cJSON_IsNumber(quality)) {
        int val = (int)quality->valuedouble;
        if (val >= 5 && val <= 50) {
            nvs_set_u8(nvs, NVS_KEY_QUALITY, (uint8_t)val);
            ESP_LOGI(TAG, "Config: jpeg_quality=%d", val);
        }
    }

    // multi-dispositivo: l'app manda l'UID dell'utente e l'ID del dispositivo
    cJSON *uid = cJSON_GetObjectItem(root, "owner_uid");
    if (uid && cJSON_IsString(uid) && strlen(uid->valuestring) > 0) {
        nvs_set_str(nvs, NVS_KEY_OWNER_UID, uid->valuestring);
        ESP_LOGI(TAG, "Config: owner_uid=%.8s...", uid->valuestring);
    }
    cJSON *did = cJSON_GetObjectItem(root, "device_id");
    if (did && cJSON_IsString(did) && strlen(did->valuestring) > 0) {
        nvs_set_str(nvs, NVS_KEY_DEVICE_ID, did->valuestring);
        ESP_LOGI(TAG, "Config: device_id=%s", did->valuestring);
    }

    nvs_commit(nvs);
    nvs_close(nvs);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Configurazione salvata. Riavvio per applicare.\"}");

    return ESP_OK;
}

// ===== HANDLER: POST /reboot (riavvio remoto) =====
static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!verifica_auth(req)) return ESP_OK;

    ESP_LOGW(TAG, "Riavvio richiesto da utente autenticato");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Riavvio in corso...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ===== HANDLER: OPTIONS (preflight CORS per l'app) =====
// necessario perché l'app Android fa richieste cross-origin
static esp_err_t cors_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ===== HANDLER: POST /wifi_config (cambia WiFi senza reset completo) =====
static esp_err_t wifi_config_handler(httpd_req_t *req)
{
    if (!verifica_auth(req)) return ESP_OK;

    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body vuoto");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"JSON non valido\"}");
        return ESP_FAIL;
    }

    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(root, "wifi_pass"));

    if (!ssid || strlen(ssid) == 0) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"SSID mancante\"}");
        return ESP_FAIL;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"NVS error\"}");
        return ESP_FAIL;
    }

    nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_WIFI_PASS, pass ? pass : "");
    nvs_commit(nvs);
    nvs_close(nvs);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "WiFi config aggiornata: SSID='%s'", ssid);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"WiFi aggiornato. Riavvio...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    if (!verifica_auth(req)) return ESP_OK;

    ESP_LOGW(TAG, "Factory reset richiesto!");

    // cancella certificati TLS
    cert_manager_reset();

    // cancella NVS principale
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"Factory reset completato. Riavvio...\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ===== HANDLER: / (pagina principale) =====
static esp_err_t root_handler(httpd_req_t *req)
{
    if (!verifica_auth(req)) return ESP_OK;

    static const char html[] =
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta charset='UTF-8'>"
        "<title>SecurityCam</title>"
        "<style>"
        "body{font-family:system-ui;background:#1a1a2e;color:#e0e0e0;padding:16px;text-align:center}"
        "h1{color:#0f9ef0;margin-bottom:8px}p{color:#8899a6;margin-bottom:16px}"
        "img{max-width:100%;border-radius:8px;border:2px solid #2a3a5c}"
        "a{color:#0f9ef0;display:inline-block;margin:8px 12px;text-decoration:none}"
        "a:hover{text-decoration:underline}"
        ".links{margin-top:16px}"
        ".secure{color:#4ade80;font-size:.8rem;margin-top:8px}"
        "</style></head><body>"
        "<h1>SecurityCam</h1>"
        "<p>Stream live dalla camera ESP32-S3</p>"
        "<p class='secure'>Connessione autenticata (Basic Auth)</p>"
        "<img src='/stream' alt='Stream'>"
        "<div class='links'>"
        "<a href='/capture'>Cattura foto</a>"
        "<a href='/status'>Stato (JSON)</a>"
        "</div>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// ===== START/STOP =====
esp_err_t http_server_start(void)
{
#ifdef CONFIG_ESP_HTTPS_SERVER_ENABLE
    const char *cert = cert_manager_get_cert();
    const char *key = cert_manager_get_key();

    if (cert && key) {
        httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
        config.httpd.max_uri_handlers = 16;
        config.httpd.stack_size = 12288;
        config.httpd.core_id = 0;
        config.servercert = (const uint8_t *)cert;
        config.servercert_len = cert_manager_get_cert_len();
        config.prvtkey_pem = (const uint8_t *)key;
        config.prvtkey_len = cert_manager_get_key_len();

        esp_err_t err = httpd_ssl_start(&server, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTPS fallito: %s, fallback HTTP", esp_err_to_name(err));
            goto fallback_http;
        }
    } else {
        ESP_LOGW(TAG, "Certificati non disponibili, uso HTTP");
        goto fallback_http;
    }
    goto register_handlers;

fallback_http:
#endif
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers = 16;
        config.stack_size = 8192;
        config.core_id = 0;

        esp_err_t err = httpd_start(&server, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Avvio server fallito: %s", esp_err_to_name(err));
            return err;
        }
    }

#ifdef CONFIG_ESP_HTTPS_SERVER_ENABLE
register_handlers:
#endif

    // endpoint principali
    httpd_uri_t uri_root    = { .uri = "/",        .method = HTTP_GET,  .handler = root_handler };
    httpd_uri_t uri_stream  = { .uri = "/stream",  .method = HTTP_GET,  .handler = stream_handler };
    httpd_uri_t uri_capture = { .uri = "/capture", .method = HTTP_GET,  .handler = capture_handler };
    httpd_uri_t uri_status  = { .uri = "/status",  .method = HTTP_GET,  .handler = status_handler };
    httpd_uri_t uri_config  = { .uri = "/config",  .method = HTTP_POST, .handler = config_handler };
    httpd_uri_t uri_reboot  = { .uri = "/reboot",  .method = HTTP_POST, .handler = reboot_handler };
    httpd_uri_t uri_wifi    = { .uri = "/wifi_config", .method = HTTP_POST, .handler = wifi_config_handler };
    httpd_uri_t uri_reset   = { .uri = "/factory_reset", .method = HTTP_POST, .handler = factory_reset_handler };

    httpd_uri_t uri_cors_config = { .uri = "/config", .method = HTTP_OPTIONS, .handler = cors_handler };
    httpd_uri_t uri_cors_reboot = { .uri = "/reboot", .method = HTTP_OPTIONS, .handler = cors_handler };
    httpd_uri_t uri_cors_status = { .uri = "/status", .method = HTTP_OPTIONS, .handler = cors_handler };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_stream);
    httpd_register_uri_handler(server, &uri_capture);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_config);
    httpd_register_uri_handler(server, &uri_reboot);
    httpd_register_uri_handler(server, &uri_reset);
    httpd_register_uri_handler(server, &uri_cors_config);
    httpd_register_uri_handler(server, &uri_cors_reboot);
    httpd_register_uri_handler(server, &uri_cors_status);
    httpd_register_uri_handler(server, &uri_wifi);

    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
#ifdef CONFIG_ESP_HTTPS_SERVER_ENABLE
    if (cert_manager_get_cert()) {
        ESP_LOGI(TAG, "Server avviato su https://%s/ (HTTPS + Basic Auth)", ip);
    } else {
        ESP_LOGI(TAG, "Server avviato su http://%s/ (Basic Auth)", ip);
    }
#else
    ESP_LOGI(TAG, "Server avviato su http://%s/ (Basic Auth attiva)", ip);
#endif
    ESP_LOGI(TAG, "  Credenziali: %s / %s", HTTP_AUTH_USER, HTTP_AUTH_PASS);

    return ESP_OK;
}

void http_server_stop(void)
{
    if (server) {
#ifdef CONFIG_ESP_HTTPS_SERVER_ENABLE
        httpd_ssl_stop(server);
#else
        httpd_stop(server);
#endif
        server = NULL;
    }
}