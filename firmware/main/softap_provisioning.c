/*
 * softap_provisioning.c
 * Provisioning tramite SoftAP: l'ESP crea un hotspot WiFi,
 * l'utente si collega e apre una pagina web dove inserisce
 * le credenziali WiFi e i dati Firebase.
 * 
 * L'HTML della pagina è embeddata direttamente nel codice come
 * stringa costante (non uso SPIFFS per semplicità).
 */
#include "softap_provisioning.h"
#include "wifi_manager.h"
#include "board_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "PROV";
static httpd_handle_t server = NULL;
static esp_netif_t   *ap_netif = NULL;

// pagina HTML per la configurazione
// ho usato CSS inline per non avere file separati
static const char PAGINA_CONFIG[] =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta charset='UTF-8'>"
"<title>Security Cam Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;"
"min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}"
".card{background:#16213e;border-radius:12px;padding:28px;max-width:400px;width:100%;"
"box-shadow:0 8px 32px rgba(0,0,0,.4)}"
"h1{font-size:1.4rem;margin-bottom:6px;color:#0f9ef0}"
".sub{color:#8899a6;margin-bottom:20px;font-size:.85rem}"
"label{display:block;font-size:.82rem;color:#8899a6;margin-bottom:3px;margin-top:14px}"
"input{width:100%;padding:9px 12px;border-radius:6px;border:1px solid #2a3a5c;"
"background:#0f3460;color:#e0e0e0;font-size:.95rem;outline:none}"
"input:focus{border-color:#0f9ef0}"
".sep{margin-top:20px;padding-top:16px;border-top:1px solid #2a3a5c}"
"h2{font-size:1rem;color:#0f9ef0;margin-bottom:3px}"
".hint{color:#5c6a7a;font-size:.75rem;margin-bottom:6px}"
"button{width:100%;padding:11px;border:none;border-radius:6px;background:#0f9ef0;"
"color:white;font-size:.95rem;font-weight:600;cursor:pointer;margin-top:20px}"
"button:hover{background:#0d8bd6}"
"button:disabled{background:#2a3a5c;cursor:wait}"
"#st{margin-top:14px;padding:10px;border-radius:6px;display:none;font-size:.85rem}"
".ok{background:#0a3d2a;color:#4ade80;display:block}"
".err{background:#4a1c1c;color:#f87171;display:block}"
"</style></head><body>"
"<div class='card'>"
"<h1>Security Cam Setup</h1>"
"<p class='sub'>Inserisci i dati per configurare la camera.</p>"
"<form id='f'>"
"<label>SSID WiFi *</label>"
"<input id='ssid' required placeholder='Nome della tua rete WiFi'>"
"<label>Password WiFi</label>"
"<input id='wpass' type='password' placeholder='Password'>"
"<div class='sep'>"
"<h2>Firebase</h2>"
"<p class='hint'>Dati dal progetto Firebase (console.firebase.google.com)</p>"
"<label>Project ID *</label>"
"<input id='fbp' required placeholder='es: mio-progetto-12345'>"
"<label>Web API Key *</label>"
"<input id='fbk' required placeholder='AIzaSy...'>"
"<label>Storage Bucket *</label>"
"<input id='fbb' required placeholder='mio-progetto-12345.appspot.com'>"
"</div>"
"<button type='submit' id='btn'>Salva e Riavvia</button>"
"</form>"
"<div id='st'></div>"
"</div>"
"<script>"
"document.getElementById('f').onsubmit=async function(e){"
"e.preventDefault();"
"var b=document.getElementById('btn'),s=document.getElementById('st');"
"b.disabled=true;b.textContent='Salvataggio...';"
"s.className='';s.style.display='none';"
"try{"
"var r=await fetch('/api/config',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({"
"ssid:document.getElementById('ssid').value,"
"wifi_pass:document.getElementById('wpass').value,"
"fb_project:document.getElementById('fbp').value,"
"fb_apikey:document.getElementById('fbk').value,"
"fb_bucket:document.getElementById('fbb').value"
"})});"
"var d=await r.json();"
"if(d.ok){s.className='ok';s.textContent='Configurazione salvata! Il dispositivo si sta riavviando. Puoi chiudere questa pagina e ricollegarti alla tua rete WiFi.';b.textContent='Riavvio in corso...';}"
"else{s.className='err';s.textContent='Errore: '+(d.error||'?');b.disabled=false;b.textContent='Salva e Riavvia';}"
"}catch(x){s.className='err';s.textContent='Errore: '+x.message;b.disabled=false;b.textContent='Salva e Riavvia';}"
"};"
"</script></body></html>";

// handler per la pagina principale
static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGINA_CONFIG, HTTPD_RESP_USE_STRLEN);
}

// handler per il POST con i dati di configurazione
static esp_err_t config_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body vuoto");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    // parso il JSON
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"JSON non valido\"}");
        return ESP_FAIL;
    }

    const char *ssid      = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *wifi_pass  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "wifi_pass"));
    const char *fb_project = cJSON_GetStringValue(cJSON_GetObjectItem(root, "fb_project"));
    const char *fb_apikey  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "fb_apikey"));
    const char *fb_bucket  = cJSON_GetStringValue(cJSON_GetObjectItem(root, "fb_bucket"));

    if (!ssid || !fb_project || !fb_apikey || !fb_bucket) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Campi obbligatori mancanti\"}");
        return ESP_FAIL;
    }

    // salvo tutto su NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Errore NVS\"}");
        return ESP_FAIL;
    }

    nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_WIFI_PASS, wifi_pass ? wifi_pass : "");
    nvs_set_str(nvs, NVS_KEY_FB_PROJECT, fb_project);
    nvs_set_str(nvs, NVS_KEY_FB_APIKEY, fb_apikey);
    nvs_set_str(nvs, NVS_KEY_FB_BUCKET, fb_bucket);
    nvs_set_u8(nvs, NVS_KEY_CONFIGURED, 1);
    nvs_commit(nvs);
    nvs_close(nvs);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Configurazione salvata! SSID='%s' Firebase='%s'", ssid, fb_project);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    // riavvio in un task separato per non bloccare il contesto HTTP
    // (se fai esp_restart dentro l'handler HTTP crasha)
    xTaskCreate(
        (TaskFunction_t)(void(*)(void))esp_restart,
        "restart", 2048, NULL, 5, NULL
    );

    return ESP_OK;
}

// redirect per il captive portal (qualsiasi URL -> pagina config)
static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t provisioning_start(void)
{
    ESP_LOGI(TAG, "Avvio modalita' provisioning SoftAP...");

    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid           = SOFTAP_SSID,
            .ssid_len       = strlen(SOFTAP_SSID),
            .password       = SOFTAP_PASS,
            .channel        = SOFTAP_CHANNEL,
            .max_connection = SOFTAP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP avviato: SSID='%s' PASS='%s'", SOFTAP_SSID, SOFTAP_PASS);

    // avvio server HTTP per la pagina di config
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Server HTTP non avviato: %s", esp_err_to_name(err));
        return err;
    }

    // registro gli endpoint
    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = page_handler
    };
    httpd_uri_t uri_config = {
        .uri = "/api/config", .method = HTTP_POST, .handler = config_handler
    };
    httpd_uri_t uri_catch_all = {
        .uri = "/*", .method = HTTP_GET, .handler = redirect_handler
    };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_config);
    httpd_register_uri_handler(server, &uri_catch_all);

    ESP_LOGI(TAG, "Pagina config disponibile su http://192.168.4.1/");
    return ESP_OK;
}

void provisioning_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
}

bool provisioning_is_configured(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) return false;

    uint8_t val = 0;
    err = nvs_get_u8(nvs, NVS_KEY_CONFIGURED, &val);
    nvs_close(nvs);
    return (err == ESP_OK && val == 1);
}
