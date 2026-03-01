/*
 * Input HAL — public API
 *
 * Backends (BLE HID, USB-Serial-JTAG) post terminal-ready byte sequences
 * to a shared FreeRTOS queue.  The SSH task calls input_hal_read() without
 * caring which backend fired.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define INPUT_EVENT_MAX_LEN  8

typedef struct {
    uint8_t buf[INPUT_EVENT_MAX_LEN];
    uint8_t len;   /* 0 = empty / timeout */
} input_event_t;

/**
 * Create the shared queue and initialise enabled backends.
 * Must be called after esp_event_loop_create_default() (WiFi init does this).
 */
esp_err_t input_hal_init(void);

/**
 * Block until an input event arrives or timeout_ms elapses.
 *
 * @param ev         Output event (valid only when true is returned)
 * @param timeout_ms Milliseconds to wait; portMAX_DELAY if 0
 * @return true if an event was received, false on timeout
 */
bool input_hal_read(input_event_t *ev, uint32_t timeout_ms);
