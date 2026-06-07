/*
 * sd_card_manager.c
 * Gestione SD card come storage di fallback.
 * File salvati nella root: evt1_f0.jpg, evt1_m.txt ecc
 */
#include "sd_card_manager.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "SD";
static bool mounted = false;
static sdmmc_card_t *card = NULL;

esp_err_t sd_card_init(void)
{
    if (mounted) {
        ESP_LOGW(TAG, "SD gia' montata");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Montaggio SD card (SDMMC 1-bit)...");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0  = SD_PIN_D0;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "Impossibile montare il filesystem");
        } else {
            ESP_LOGE(TAG, "Montaggio SD fallito: %s", esp_err_to_name(err));
            ESP_LOGE(TAG, "La SD card e' inserita?");
        }
        card = NULL;
        return err;
    }

    mounted = true;
    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD montata su %s", SD_MOUNT_POINT);
    return ESP_OK;
}

void sd_card_deinit(void)
{
    if (mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        mounted = false;
        card = NULL;
        ESP_LOGI(TAG, "SD smontata");
    }
}

bool sd_card_is_available(void)
{
    if (!mounted) return false;

    // provo a scrivere e leggere un byte per verificare che il bus funziona
    // stat() non basta, non rileva errori I/O sul bus SDMMC
    FILE *f = fopen(SD_MOUNT_POINT "/chk.tmp", "w");
    if (!f) {
        ESP_LOGW(TAG, "SD card: scrittura test fallita (errno=%d)", errno);
        mounted = false;
        return false;
    }
    int ret = fputc('X', f);
    fclose(f);

    if (ret == EOF) {
        ESP_LOGW(TAG, "SD card: scrittura fallita");
        mounted = false;
        return false;
    }

    // se la scrittura è andata, cancello il file di test
    remove(SD_MOUNT_POINT "/chk.tmp");
    return true;
}

esp_err_t sd_card_save_frame(const uint8_t *data, size_t length,
                             uint32_t event_id, int frame_idx)
{
    if (!mounted || !data) return ESP_ERR_INVALID_STATE;
    // Crea sottocartella per ogni blocco di 100 eventi
    char dir[40];
    snprintf(dir, sizeof(dir), SD_MOUNT_POINT "/%lu",
             (unsigned long)(event_id / 100) * 100);
    mkdir(dir, 0775);  // ignora errore se esiste già

    char path[80];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/evt%lu_f%d.jpg",
             (unsigned long)event_id, frame_idx);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Impossibile aprire %s (errno=%d)", path, errno);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, length, f);
    fclose(f);

    if (written != length) {
        ESP_LOGE(TAG, "Scrittura incompleta: %u/%u bytes", (unsigned)written, (unsigned)length);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Salvato %s (%u bytes)", path, (unsigned)length);
    return ESP_OK;
}

esp_err_t sd_card_save_metadata(uint32_t event_id, const char *json_str)
{
    if (!mounted || !json_str) return ESP_ERR_INVALID_STATE;

    char path[80];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/evt%lu_m.txt",
             (unsigned long)event_id);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Impossibile aprire %s (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    fputs(json_str, f);
    fclose(f);

    ESP_LOGI(TAG, "Metadata salvato: %s", path);
    return ESP_OK;
}

uint64_t sd_card_get_free_bytes(void)
{
    if (!mounted) return 0;

    FATFS *fs;
    DWORD free_clust;
    FRESULT res = f_getfree("0:", &free_clust, &fs);
    if (res != FR_OK) return 0;

    return (uint64_t)free_clust * fs->csize * 512;
}
