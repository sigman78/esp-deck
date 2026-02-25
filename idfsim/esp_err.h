/* sim/idf_stubs/esp_err.h — minimal ESP-IDF error type definitions. */
#pragma once
#include <stdint.h>

typedef int32_t esp_err_t;

#define ESP_OK                0
#define ESP_FAIL             -1
#define ESP_ERR_NO_MEM        0x00000101
#define ESP_ERR_INVALID_ARG   0x00000102
#define ESP_ERR_INVALID_STATE 0x00000103
