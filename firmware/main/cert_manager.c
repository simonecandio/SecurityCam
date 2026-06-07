/*
 * cert_manager.c
 * Genera e gestisce certificati TLS self-signed per HTTPS.
 * Al primo boot genera una chiave RSA 2048 e un certificato X.509
 * usando mbedtls, poi li salva in NVS per i boot successivi.
 */
#include "cert_manager.h"
#include "board_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CERTS";

#define NVS_KEY_CERT "tls_cert"
#define NVS_KEY_PKEY "tls_pkey"
#define CERT_BUF_SIZE 2048
#define KEY_BUF_SIZE 2048

static char *cert_pem = NULL;
static char *key_pem = NULL;
static size_t cert_len = 0;
static size_t key_len = 0;

/**
 * Genera chiave RSA 2048 + certificato self-signed X.509.
 * Questa operazione richiede 15-30 secondi su ESP32-S3.
 */
static esp_err_t genera_certificato(void)
{
    ESP_LOGW(TAG, "=== Generazione certificato TLS (prima volta, ~20 secondi)... ===");

    int ret;
    mbedtls_pk_context key_ctx;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_pk_init(&key_ctx);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    // seed del generatore random
    const char *pers = "securitycam_cert_gen";
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "ctr_drbg_seed fallito: -0x%04x", -ret);
        goto cleanup;
    }

    // genera chiave RSA 2048
    ESP_LOGI(TAG, "Generazione chiave RSA 2048 bit...");
    ret = mbedtls_pk_setup(&key_ctx, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) {
        ESP_LOGE(TAG, "pk_setup fallito: -0x%04x", -ret);
        goto cleanup;
    }

    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key_ctx), mbedtls_ctr_drbg_random,
                               &ctr_drbg, 2048, 65537);
    if (ret != 0) {
        ESP_LOGE(TAG, "rsa_gen_key fallito: -0x%04x", -ret);
        goto cleanup;
    }
    ESP_LOGI(TAG, "Chiave RSA generata!");

    // esporta chiave privata in PEM
    key_pem = (char *)calloc(1, KEY_BUF_SIZE);
    if (!key_pem) { ret = -1; goto cleanup; }

    ret = mbedtls_pk_write_key_pem(&key_ctx, (unsigned char *)key_pem, KEY_BUF_SIZE);
    if (ret != 0) {
        ESP_LOGE(TAG, "pk_write_key_pem fallito: -0x%04x", -ret);
        free(key_pem); key_pem = NULL;
        goto cleanup;
    }
    key_len = strlen(key_pem) + 1;

    // configura certificato X.509
    ESP_LOGI(TAG, "Generazione certificato X.509...");
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    unsigned char serial_buf[] = { 0x01 };
    ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial_buf, sizeof(serial_buf));
    if (ret != 0) goto cleanup;

    // subject e issuer (self-signed: sono uguali)
    ret = mbedtls_x509write_crt_set_subject_name(&crt,
        "CN=SecurityCam-ESP32,O=SecurityCam,C=IT");
    if (ret != 0) { ESP_LOGE(TAG, "set_subject fallito"); goto cleanup; }

    ret = mbedtls_x509write_crt_set_issuer_name(&crt,
        "CN=SecurityCam-ESP32,O=SecurityCam,C=IT");
    if (ret != 0) { ESP_LOGE(TAG, "set_issuer fallito"); goto cleanup; }

    // validita
    ret = mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000");
    if (ret != 0) { ESP_LOGE(TAG, "set_validity fallito"); goto cleanup; }

    // assegna la chiave
    mbedtls_x509write_crt_set_subject_key(&crt, &key_ctx);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key_ctx);

    // basic constraints: CA=true (self-signed)
    ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1);
    if (ret != 0) { ESP_LOGE(TAG, "set_basic_constraints fallito"); goto cleanup; }

    // esporta certificato in PEM
    cert_pem = (char *)calloc(1, CERT_BUF_SIZE);
    if (!cert_pem) { ret = -1; goto cleanup; }

    ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)cert_pem, CERT_BUF_SIZE,
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "x509write_crt_pem fallito: -0x%04x", -ret);
        free(cert_pem); cert_pem = NULL;
        goto cleanup;
    }
    cert_len = strlen(cert_pem) + 1;

    ESP_LOGI(TAG, "Certificato generato! (cert=%d bytes, key=%d bytes)",
             (int)cert_len, (int)key_len);

cleanup:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key_ctx);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);

    return (ret == 0 && cert_pem && key_pem) ? ESP_OK : ESP_FAIL;
}

static esp_err_t salva_in_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tls_certs", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(nvs, NVS_KEY_CERT, cert_pem, cert_len);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    err = nvs_set_blob(nvs, NVS_KEY_PKEY, key_pem, key_len);
    if (err != ESP_OK) { nvs_close(nvs); return err; }

    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Certificati salvati in NVS");
    return ESP_OK;
}

static esp_err_t carica_da_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tls_certs", NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    // leggi dimensioni
    size_t c_len = 0, k_len = 0;
    err = nvs_get_blob(nvs, NVS_KEY_CERT, NULL, &c_len);
    if (err != ESP_OK || c_len == 0) { nvs_close(nvs); return ESP_ERR_NOT_FOUND; }

    err = nvs_get_blob(nvs, NVS_KEY_PKEY, NULL, &k_len);
    if (err != ESP_OK || k_len == 0) { nvs_close(nvs); return ESP_ERR_NOT_FOUND; }

    // alloca e leggi
    cert_pem = (char *)calloc(1, c_len);
    key_pem = (char *)calloc(1, k_len);
    if (!cert_pem || !key_pem) {
        if (cert_pem) { free(cert_pem); cert_pem = NULL; }
        if (key_pem) { free(key_pem); key_pem = NULL; }
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }

    nvs_get_blob(nvs, NVS_KEY_CERT, cert_pem, &c_len);
    nvs_get_blob(nvs, NVS_KEY_PKEY, key_pem, &k_len);
    cert_len = c_len;
    key_len = k_len;

    nvs_close(nvs);
    ESP_LOGI(TAG, "Certificati caricati da NVS (cert=%d, key=%d)", (int)cert_len, (int)key_len);
    return ESP_OK;
}

esp_err_t cert_manager_init(void)
{
    // prova a caricare da NVS
    esp_err_t err = carica_da_nvs();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Certificati esistenti trovati in NVS");
        return ESP_OK;
    }

    // primo boot: genera
    ESP_LOGW(TAG, "Nessun certificato in NVS, generazione in corso...");
    err = genera_certificato();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Generazione certificato fallita!");
        return err;
    }

    // salva per i prossimi boot
    err = salva_in_nvs();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Salvataggio NVS fallito, il certificato sara rigenerato al prossimo boot");
    }

    return ESP_OK;
}

const char *cert_manager_get_cert(void) { return cert_pem; }
const char *cert_manager_get_key(void) { return key_pem; }
size_t cert_manager_get_cert_len(void) { return cert_len; }
size_t cert_manager_get_key_len(void) { return key_len; }

esp_err_t cert_manager_reset(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tls_certs", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_erase_key(nvs, NVS_KEY_CERT);
    nvs_erase_key(nvs, NVS_KEY_PKEY);
    nvs_commit(nvs);
    nvs_close(nvs);

    if (cert_pem) { free(cert_pem); cert_pem = NULL; }
    if (key_pem) { free(key_pem); key_pem = NULL; }
    cert_len = 0;
    key_len = 0;

    ESP_LOGW(TAG, "Certificati cancellati da NVS, verranno rigenerati al prossimo boot");
    return ESP_OK;
}