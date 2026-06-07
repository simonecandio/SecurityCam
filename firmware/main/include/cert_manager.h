#ifndef CERT_MANAGER_H
#define CERT_MANAGER_H

#include "esp_err.h"

/**
 * Inizializza i certificati TLS per il server HTTPS.
 * Al primo boot genera chiave RSA 2048 + certificato self-signed
 * e li salva in NVS. Ai boot successivi li legge da NVS.
 * ATTENZIONE: la prima generazione richiede 15-30 secondi.
 */
esp_err_t cert_manager_init(void);

/** Restituisce il certificato PEM (NULL terminated) */
const char *cert_manager_get_cert(void);

/** Restituisce la chiave privata PEM (NULL terminated) */
const char *cert_manager_get_key(void);

/** Dimensione del certificato PEM incluso il terminatore */
size_t cert_manager_get_cert_len(void);

/** Dimensione della chiave PEM incluso il terminatore */
size_t cert_manager_get_key_len(void);

/** Cancella i certificati da NVS (per rigenerazione al prossimo boot) */
esp_err_t cert_manager_reset(void);

#endif