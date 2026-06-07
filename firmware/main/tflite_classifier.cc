/*
 * tflite_classifier.cc
 * Micro classificatore TFLite per ESP32-S3
 *
 * Supporta sia modelli float32 che int8/uint8.
 * Il modello float evita i bug di quantizzazione su esp-tflite-micro.
 */

#include "tflite_classifier.h"
#include "micro_model_data.h"

// TFLite Micro
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ESP-IDF
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

// JPEG decoder dalla esp32-camera
#include "img_converters.h"
#include "esp_camera.h"

#include <cstring>
#include <cstdlib>

static const char *TAG = "TFLITE";

#define TENSOR_ARENA_SIZE   (700 * 1024)  // 600 KB per modello float32
#define MODEL_INPUT_SIZE    96
#define MODEL_INPUT_CH      3
#define NUM_CLASSES         3

// Risoluzione camera (VGA)
#define CAM_WIDTH  640
#define CAM_HEIGHT 480

static uint8_t *tensor_arena = nullptr;
static tflite::MicroInterpreter *interpreter = nullptr;
static TfLiteTensor *input_tensor = nullptr;
static TfLiteTensor *output_tensor = nullptr;
static bool initialized = false;

// ============================================================
// Inizializzazione
// ============================================================

esp_err_t tflite_classifier_init(void)
{
    if (initialized) return ESP_OK;

    ESP_LOGI(TAG, "Inizializzazione classificatore TFLite Micro...");
    ESP_LOGI(TAG, "  Modello: %d bytes (%.0f KB)", MICRO_MODEL_SIZE,
             MICRO_MODEL_SIZE / 1024.0f);

    // 1. Tensor arena in PSRAM (azzerata)
    tensor_arena = (uint8_t *)heap_caps_aligned_alloc(16, TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM);
    if (!tensor_arena) {
        ESP_LOGE(TAG, "Impossibile allocare tensor arena in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    memset(tensor_arena, 0, TENSOR_ARENA_SIZE);

    // 2. Carica modello
    const tflite::Model *model = tflite::GetModel(micro_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Versione schema non corrispondente");
        heap_caps_free(tensor_arena);
        tensor_arena = nullptr;
        return ESP_FAIL;
    }

    // 3. Registra operazioni per MobileNetV2 (float e int8)
    static tflite::MicroMutableOpResolver<21> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddAdd();
    resolver.AddRelu6();
    resolver.AddRelu();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddLogistic();           // sigmoid
    resolver.AddMean();               // GlobalAveragePooling
    resolver.AddAveragePool2D();      // alternativa a Mean
    resolver.AddPad();
    resolver.AddPadV2();
    resolver.AddQuantize();
    resolver.AddDequantize();
    resolver.AddSoftmax();
    resolver.AddMul();
    resolver.AddSub();
    resolver.AddExpandDims();
    resolver.AddSqueeze();
    resolver.AddResizeBilinear();

    // 4. Crea interprete
    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, TENSOR_ARENA_SIZE);
    interpreter = &static_interpreter;

    // 5. Alloca tensori
    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors fallito");
        heap_caps_free(tensor_arena);
        tensor_arena = nullptr;
        return ESP_FAIL;
    }

    input_tensor = interpreter->input(0);
    output_tensor = interpreter->output(0);

    const char *type_name = "unknown";
    if (input_tensor->type == kTfLiteFloat32) type_name = "float32";
    else if (input_tensor->type == kTfLiteUInt8) type_name = "uint8";
    else if (input_tensor->type == kTfLiteInt8) type_name = "int8";

    ESP_LOGI(TAG, "  Input:  [%d, %d, %d, %d] tipo=%s",
             input_tensor->dims->data[0], input_tensor->dims->data[1],
             input_tensor->dims->data[2], input_tensor->dims->data[3],
             type_name);
    ESP_LOGI(TAG, "  Output: [%d, %d] tipo=%s",
             output_tensor->dims->data[0], output_tensor->dims->data[1],
             type_name);

    size_t used = interpreter->arena_used_bytes();
    ESP_LOGI(TAG, "  Arena: %u/%d KB (%.0f%%)",
             (unsigned)used / 1024, TENSOR_ARENA_SIZE / 1024,
             (float)used / TENSOR_ARENA_SIZE * 100);

    // 6. Test invoke con dati dummy
    ESP_LOGI(TAG, "  Test inferenza con dati dummy...");
    if (input_tensor->type == kTfLiteFloat32) {
        for (size_t i = 0; i < input_tensor->bytes / sizeof(float); i++) {
            input_tensor->data.f[i] = 0.5f;
        }
    } else {
        memset(input_tensor->data.uint8, 128, input_tensor->bytes);
    }

    TfLiteStatus invoke_status = interpreter->Invoke();
    if (invoke_status != kTfLiteOk) {
        ESP_LOGE(TAG, "Test invoke FALLITO! Modello incompatibile.");
        heap_caps_free(tensor_arena);
        tensor_arena = nullptr;
        interpreter = nullptr;
        return ESP_FAIL;
    }

    if (output_tensor->type == kTfLiteFloat32) {
        ESP_LOGI(TAG, "  Test OK: person=%.2f cat=%.2f dog=%.2f",
                 output_tensor->data.f[0], output_tensor->data.f[1],
                 output_tensor->data.f[2]);
    } else {
        ESP_LOGI(TAG, "  Test OK: person=%.2f cat=%.2f dog=%.2f",
                 output_tensor->data.uint8[0] / 255.0f,
                 output_tensor->data.uint8[1] / 255.0f,
                 output_tensor->data.uint8[2] / 255.0f);
    }

    initialized = true;
    ESP_LOGI(TAG, "Classificatore TFLite pronto!");
    return ESP_OK;
}

// ============================================================
// Resize nearest-neighbor
// ============================================================

static void resize_rgb888(const uint8_t *src, int src_w, int src_h,
                          uint8_t *dst, int dst_w, int dst_h)
{
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = dy * src_h / dst_h;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = dx * src_w / dst_w;
            int si = (sy * src_w + sx) * 3;
            int di = (dy * dst_w + dx) * 3;
            dst[di]     = src[si];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
        }
    }
}

// ============================================================
// Classificazione
// ============================================================

esp_err_t tflite_classify_frame(const uint8_t *jpeg_data, size_t jpeg_len,
                                 classify_result_t *result)
{
    if (!initialized || !jpeg_data || !result) return ESP_ERR_INVALID_STATE;

    int64_t t_start = esp_timer_get_time();

    // Step 1: decode JPEG -> RGB888
    size_t rgb_size = CAM_WIDTH * CAM_HEIGHT * 3;
    uint8_t *rgb = (uint8_t *)heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        ESP_LOGE(TAG, "Impossibile allocare buffer RGB (%u KB)",
                 (unsigned)(rgb_size / 1024));
        return ESP_ERR_NO_MEM;
    }

    bool decode_ok = fmt2rgb888(jpeg_data, jpeg_len, PIXFORMAT_JPEG, rgb);
    if (!decode_ok) {
        ESP_LOGE(TAG, "Decodifica JPEG fallita");
        heap_caps_free(rgb);
        return ESP_FAIL;
    }

    int64_t t_decode = esp_timer_get_time();

    // Step 2: resize da 640x480 a 96x96
    int pixel_count = MODEL_INPUT_SIZE * MODEL_INPUT_SIZE * MODEL_INPUT_CH;
    uint8_t *resized = (uint8_t *)heap_caps_malloc(pixel_count, MALLOC_CAP_SPIRAM);
    if (!resized) {
        heap_caps_free(rgb);
        return ESP_ERR_NO_MEM;
    }

    resize_rgb888(rgb, CAM_WIDTH, CAM_HEIGHT,
                  resized, MODEL_INPUT_SIZE, MODEL_INPUT_SIZE);
    heap_caps_free(rgb);

    // Copia nel tensor di input con il tipo corretto
    if (input_tensor->type == kTfLiteFloat32) {
        // Float model: normalizza 0-255 -> 0.0-1.0
        for (int i = 0; i < pixel_count; i++) {
            input_tensor->data.f[i] = resized[i] / 255.0f;
        }
    } else {
        // Int8/uint8 model: copia direttamente
        memcpy(input_tensor->data.uint8, resized, pixel_count);
    }
    heap_caps_free(resized);

    int64_t t_resize = esp_timer_get_time();

    // Step 3: inferenza
    if (interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Inferenza fallita");
        return ESP_FAIL;
    }

    int64_t t_infer = esp_timer_get_time();

    // Step 4: leggi output
    if (output_tensor->type == kTfLiteFloat32) {
        result->person = output_tensor->data.f[0];
        result->cat    = output_tensor->data.f[1];
        result->dog    = output_tensor->data.f[2];
    } else {
        result->person = output_tensor->data.uint8[0] / 255.0f;
        result->cat    = output_tensor->data.uint8[1] / 255.0f;
        result->dog    = output_tensor->data.uint8[2] / 255.0f;
    }

    ESP_LOGI(TAG, "Classificazione: person=%.2f cat=%.2f dog=%.2f "
             "[decode=%lldms resize=%lldms infer=%lldms tot=%lldms]",
             result->person, result->cat, result->dog,
             (t_decode - t_start) / 1000,
             (t_resize - t_decode) / 1000,
             (t_infer - t_resize) / 1000,
             (t_infer - t_start) / 1000);

    return ESP_OK;
}

// ============================================================
// Filtro: confronta con categorie in NVS
// ============================================================

bool tflite_should_upload(const classify_result_t *result, float threshold)
{
    if (!result) return true;

    nvs_handle_t nvs;
    bool mon_person = true, mon_cat = false, mon_dog = false;

    if (nvs_open("security", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(nvs, "mon_person", &val) == ESP_OK) mon_person = (val != 0);
        if (nvs_get_u8(nvs, "mon_cat", &val) == ESP_OK) mon_cat = (val != 0);
        if (nvs_get_u8(nvs, "mon_dog", &val) == ESP_OK) mon_dog = (val != 0);
        nvs_close(nvs);
    }

    bool dominated = false;
    if (mon_person && result->person >= threshold) {
        ESP_LOGI(TAG, "FILTRO: person=%.2f >= %.2f -> UPLOAD", result->person, threshold);
        dominated = true;
    }
    if (mon_cat && result->cat >= threshold) {
        ESP_LOGI(TAG, "FILTRO: cat=%.2f >= %.2f -> UPLOAD", result->cat, threshold);
        dominated = true;
    }
    if (mon_dog && result->dog >= threshold) {
        ESP_LOGI(TAG, "FILTRO: dog=%.2f >= %.2f -> UPLOAD", result->dog, threshold);
        dominated = true;
    }
    if (!dominated) {
        ESP_LOGI(TAG, "FILTRO: nessuna categoria sopra soglia -> SKIP");
    }
    return dominated;
}

// ============================================================
// Cleanup
// ============================================================

void tflite_classifier_deinit(void)
{
    if (tensor_arena) {
        heap_caps_free(tensor_arena);
        tensor_arena = nullptr;
    }
    interpreter = nullptr;
    input_tensor = nullptr;
    output_tensor = nullptr;
    initialized = false;
    ESP_LOGI(TAG, "Classificatore deinizializzato");
}