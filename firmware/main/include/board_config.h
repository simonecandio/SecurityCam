/*
 * board_config.h
 * Configurazione pin e costanti per Freenove ESP32-S3-WROOM
 * 
 * Pin della camera presi dal repo ufficiale Freenove:
 * https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board
 * 
 * ATTENZIONE: i pin della camera NON si possono usare per altro
 * quando la camera è attiva (controllare sempre il pinout)
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// ===== PIN CAMERA OV2640 =====
// (dal pinout Freenove, diversi dall'AI-Thinker!!)
#define CAM_PIN_PWDN    -1   // non collegato
#define CAM_PIN_RESET   -1   // non collegato
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4   // I2C SDA
#define CAM_PIN_SIOC     5   // I2C SCL
#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2       8
#define CAM_PIN_D1       9
#define CAM_PIN_D0      11
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13
#define CAM_XCLK_FREQ   20000000  // 20 MHz

// ===== SD CARD (slot sul retro della board) =====
// SDMMC 1-bit mode, pin fissi sulla Freenove
#define SD_PIN_CMD      38
#define SD_PIN_CLK      39
#define SD_PIN_D0       40

// ===== PIR SENSOR =====
// HC-SR501: segnale su GPIO1, alimentazione 3.3V
// NOTA: ho scelto GPIO1 perché non è usato dalla camera
// e supporta interrupt. Se non funziona provare GPIO3 o GPIO14
#define PIR_GPIO_PIN     1

// ===== LED =====
#define LED_STATUS_PIN    2   // LED sulla board (GPIO2)
// c'è anche un WS2812 su GPIO48 ma per ora uso solo il led normale

// ===== SOFTAP PROVISIONING =====
#define SOFTAP_SSID            "SecurityCam-Setup"
#define SOFTAP_PASS            "setup1234"
#define SOFTAP_CHANNEL         1
#define SOFTAP_MAX_CONN        2

// ===== PARAMETRI CATTURA =====
#define CAPTURE_FRAME_COUNT    3       // quanti frame per ogni evento
#define CAPTURE_INTERVAL_MS    500     // ms tra un frame e l'altro
#define CAPTURE_QUALITY        12      // qualità JPEG (0-63, più basso = meglio)

// ===== PIR =====
#define PIR_COOLDOWN_MS        10000   // 10 secondi di cooldown tra eventi
#define PIR_DEBOUNCE_MS        200     // debounce

// ===== BUFFER EVENTI =====
#define MAX_PENDING_EVENTS     10
#define MAX_FRAME_SIZE         (80*1024)  // 80KB massimo per frame JPEG

// ===== ERROR MONITOR =====
#define ERROR_CHECK_INTERVAL_MS 5000   // controlla periferiche ogni 5s

// ===== CHIAVI NVS =====
// uso NVS per salvare le credenziali wifi e firebase
// così non devo hardcodarle nel codice
#define NVS_NAMESPACE          "sec_cam"
#define NVS_KEY_WIFI_SSID      "wifi_ssid"
#define NVS_KEY_WIFI_PASS      "wifi_pass"
#define NVS_KEY_FB_PROJECT     "fb_project"
#define NVS_KEY_FB_APIKEY      "fb_apikey"
#define NVS_KEY_FB_BUCKET      "fb_bucket"
#define NVS_KEY_CONFIGURED     "configured"

// mount point per la SD
#define SD_MOUNT_POINT "/sdcard"

// ===== SNTP (sincronizzazione orario) =====
#define SNTP_SERVER            "pool.ntp.org"
// timezone Italia: CET-1CEST,M3.5.0,M10.5.0/3
#define TIMEZONE               "CET-1CEST,M3.5.0,M10.5.0/3"

// ===== RISPARMIO ENERGETICO =====
// light sleep durante l'idle (il PIR sveglia l'ESP tramite GPIO wakeup)
#define POWER_SAVE_ENABLED     1
// NVS keys per configurazione remota
#define NVS_KEY_COOLDOWN       "cooldown_ms"
#define NVS_KEY_FRAME_COUNT    "frame_count"
#define NVS_KEY_QUALITY        "jpeg_quality"

// ===== MULTI-DISPOSITIVO =====
// L'app Android imposta questi valori via POST /config
// per associare l'ESP32 a un utente e dispositivo specifico.
// Firestore path: users/{owner_uid}/devices/{device_id}/events/...
#define NVS_KEY_OWNER_UID      "owner_uid"
#define NVS_KEY_DEVICE_ID      "device_id"
#endif
