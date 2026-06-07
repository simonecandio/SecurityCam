/*
 * tflite_classifier.h
 * Micro classificatore TFLite per filtro pre-upload
 */
#ifndef TFLITE_CLASSIFIER_H
#define TFLITE_CLASSIFIER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float person;
    float cat;
    float dog;
} classify_result_t;

esp_err_t tflite_classifier_init(void);

esp_err_t tflite_classify_frame(const uint8_t *jpeg_data, size_t jpeg_len,
                                 classify_result_t *result);

bool tflite_should_upload(const classify_result_t *result, float threshold);

void tflite_classifier_deinit(void);

#ifdef __cplusplus
}
#endif

#endif