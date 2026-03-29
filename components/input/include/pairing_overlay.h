#pragma once
#include "esp_err.h"
#include "input_hal.h"

/**
 * Run the BLE pairing overlay (blocking).
 *
 * Renders a device list modal over the terminal using ANSI sequences.
 * Blocks until the user selects a device, dismisses (Escape), or
 * times out after PAIRING_OVERLAY_TIMEOUT_MS with no new scan result.
 *
 * Call only from a task that owns the terminal — do NOT call concurrently.
 *
 * @return ESP_OK        — device selected and connection initiated
 *         ESP_ERR_TIMEOUT — timed out with no selection
 *         ESP_ERR_NOT_FOUND — user dismissed without selecting
 */
esp_err_t pairing_overlay_run(void);
