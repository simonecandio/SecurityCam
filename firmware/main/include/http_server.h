/*
 * http_server.h
 * Server HTTP: stream MJPEG, cattura singola, stato JSON
 */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

esp_err_t http_server_start(void);
void http_server_stop(void);

#endif
