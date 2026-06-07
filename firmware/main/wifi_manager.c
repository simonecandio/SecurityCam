/*
 * wifi_manager.c
 * Gestione connessione WiFi con auto-reconnect.
 * Le credenziali vengono lette da NVS (salvate tramite provisioning).
 */
#include "wifi_manager.h"
#include "board_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           10

static EventGroupHandle_t s_wifi_events = NULL;
static esp_netif_t       *s_netif       = NULL;
static bool               s_init_done   = false;
static int                s_retry_num   = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "STA avviato, connessione...");
            esp_wifi_connect();
        }
        else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "Disconnesso (reason=%d)", evt->reason);

            if (s_retry_num < MAX_RETRY) {
                s_retry_num++;
                ESP_LOGI(TAG, "Tentativo riconnessione %d/%d...", s_retry_num, MAX_RETRY);
                // backoff: aspetto sempre di più tra un tentativo e l'altro
                vTaskDelay(pdMS_TO_TICKS(1000 * s_retry_num));
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "Numero massimo tentativi raggiunto");
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            }
        }
        else if (id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "Associato all'AP");
            s_retry_num = 0;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Indirizzo IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_init_done) {
        ESP_LOGW(TAG, "WiFi gia' inizializzato");
        return ESP_OK;
    }

    // leggo le credenziali da NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS aperto fallito: nessuna credenziale salvata");
        return ESP_ERR_NOT_FOUND;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);

    err = nvs_get_str(nvs, NVS_KEY_WIFI_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }
    nvs_get_str(nvs, NVS_KEY_WIFI_PASS, pass, &pass_len);
    nvs_close(nvs);

    ESP_LOGI(TAG, "SSID trovato: '%s'", ssid);

    // inizializzo lo stack di rete (esp_netif_init già fatto in main)
    s_wifi_events = xEventGroupCreate();
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // registro gli handler per gli eventi wifi e ip
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // configuro la modalità station
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;  // WPA invece di WPA2 per compatibilità hotspot
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_init_done = true;

    // aspetto la connessione con timeout di 30 secondi
    ESP_LOGI(TAG, "In attesa di connessione WiFi...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connesso!");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Connessione WiFi FALLITA");
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "Timeout connessione WiFi");
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, NVS_KEY_WIFI_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_WIFI_PASS, password ? password : "");
    err = nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Credenziali salvate (SSID='%s')", ssid);
    return err;
}

bool wifi_manager_is_connected(void)
{
    if (!s_wifi_events) return false;
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

void wifi_manager_get_ip(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (!s_netif) return;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_netif, &ip_info) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    }
}

void wifi_manager_stop(void)
{
    if (s_init_done) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        s_init_done = false;
    }
}
