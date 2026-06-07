/*
 * sd_card_manager.h
 * Gestione SD card per salvataggio locale (fallback)
 */
#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

esp_err_t sd_card_init(void);
void sd_card_deinit(void);
bool sd_card_is_available(void);
esp_err_t sd_card_save_frame(const uint8_t *data, size_t length,
                             uint32_t event_id, int frame_idx);
esp_err_t sd_card_save_metadata(uint32_t event_id, const char *json_str);
uint64_t sd_card_get_free_bytes(void);

#endif
