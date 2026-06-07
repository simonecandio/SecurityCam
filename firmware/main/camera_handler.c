/*
 * camera_handler.c
 * Inizializzazione e cattura frame dalla OV2640
 * 
 * I pin sono specifici della Freenove ESP32-S3-WROOM, li ho presi
 * dal repo GitHub ufficiale e verificati con ESPHome community.
 * Se cambiate board dovete cambiare i pin in board_config.h
 */
#include "camera_handler.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CAM";
static bool camera_inizializzata = false;

esp_err_t camera_init(void)
{
    if (camera_inizializzata) {
        ESP_LOGW(TAG, "Camera gia' inizializzata");
        return ESP_OK;
    }

    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,

        .xclk_freq_hz = CAM_XCLK_FREQ,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_VGA,    // 640x480, buon compromesso qualità/dimensione
        .jpeg_quality = CAPTURE_QUALITY,   // 12 = qualita discreta senza occupare troppo
        .fb_count     = 2,                 // doppio buffer per cattura più fluida
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    ESP_LOGI(TAG, "Inizializzazione camera...");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init FALLITA: %s (0x%x)", esp_err_to_name(err), err);
        ESP_LOGE(TAG, "Controllare il cavo flat della camera!!");
        return err;
    }

    // regolazioni per ambiente interno (luce artificiale)
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 1);    // leggermente più luminoso
        s->set_contrast(s, 1);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);      // auto white balance
        s->set_awb_gain(s, 1);
        s->set_wb_mode(s, 0);       // 0=auto
        s->set_exposure_ctrl(s, 1); // auto exposure
        s->set_aec2(s, 1);
        s->set_gain_ctrl(s, 1);     // auto gain
        s->set_agc_gain(s, 0);
        s->set_gainceiling(s, (gainceiling_t)6);
        s->set_bpc(s, 1);
        s->set_wpc(s, 1);
        s->set_raw_gma(s, 1);
        s->set_lenc(s, 1);          // lens correction
        s->set_hmirror(s, 0);
        s->set_vflip(s, 0);
        s->set_dcw(s, 1);
        ESP_LOGI(TAG, "Sensore rilevato: PID=0x%02X", s->id.PID);
    }

    camera_inizializzata = true;
    ESP_LOGI(TAG, "Camera OK (VGA, JPEG quality=%d)", CAPTURE_QUALITY);
    return ESP_OK;
}

void camera_deinit(void)
{
    if (camera_inizializzata) {
        esp_camera_deinit();
        camera_inizializzata = false;
        ESP_LOGI(TAG, "Camera deinizializzata");
    }
}

esp_err_t camera_capture_frame(captured_frame_t *frame)
{
    if (!camera_inizializzata) {
        ESP_LOGE(TAG, "Camera non inizializzata!");
        return ESP_ERR_INVALID_STATE;
    }
    if (!frame) return ESP_ERR_INVALID_ARG;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Cattura frame fallita! Camera scollegata?");
        return ESP_FAIL;
    }

    // copio il frame in un buffer separato cosi posso restituire
    // il framebuffer alla camera subito
    frame->data = (uint8_t *)malloc(fb->len);
    if (!frame->data) {
        ESP_LOGE(TAG, "malloc fallita per %u bytes", (unsigned)fb->len);
        esp_camera_fb_return(fb);
        return ESP_ERR_NO_MEM;
    }

    memcpy(frame->data, fb->buf, fb->len);
    frame->length = fb->len;
    frame->timestamp = esp_timer_get_time() / 1000; // converti in ms

    esp_camera_fb_return(fb);

    // ESP_LOGD(TAG, "Frame catturato: %u bytes", (unsigned)frame->length);
    return ESP_OK;
}

int camera_capture_burst(captured_frame_t *frames, int count, int interval_ms)
{
    if (!frames || count <= 0) return 0;

    int captured = 0;
    for (int i = 0; i < count; i++) {
        esp_err_t err = camera_capture_frame(&frames[i]);
        if (err == ESP_OK) {
            captured++;
            ESP_LOGI(TAG, "Burst frame %d/%d: %u bytes", i+1, count, (unsigned)frames[i].length);
        } else {
            ESP_LOGW(TAG, "Burst frame %d/%d FALLITO", i+1, count);
            frames[i].data = NULL;
            frames[i].length = 0;
        }

        // aspetto tra un frame e l'altro (tranne dopo l'ultimo)
        if (i < count - 1 && interval_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }

    ESP_LOGI(TAG, "Burst completato: %d/%d frame OK", captured, count);
    return captured;
}

bool camera_health_check(void)
{
    if (!camera_inizializzata) {
        ESP_LOGW(TAG, "Health check: camera non inizializzata, riprovo...");
        // Deinit forzato per ripulire lo stato DMA/I2S residuo,
        // altrimenti la reinit produce EV-VSYNC-OVF infiniti
        esp_camera_deinit();
        vTaskDelay(pdMS_TO_TICKS(500));
        if (camera_init() != ESP_OK) {
            return false;
        }
    }
    

    // provo a fare una cattura di test
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Health check FALLITO: impossibile catturare frame");
        camera_inizializzata = false; // forzo reinit al prossimo tentativo
        return false;
    }

    bool ok = (fb->len > 0);
    esp_camera_fb_return(fb);

    if (!ok) {
        ESP_LOGE(TAG, "Health check FALLITO: frame vuoto");
        esp_camera_deinit();
        camera_inizializzata = false;
    }

    return ok;
}
