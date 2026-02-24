/*
 * BLE keyboard (HID over GATT) implementation
 *
 * TODO: Implement BLE HID host functionality
 * This is a placeholder stub for the POC
 */

#include "ble_keyboard.h"
#include "esp_log.h"

static const char *TAG = "ble_keyboard";

static bool connected = false;
static hid_keyboard_report_t last_report = {0};

/**
 * Initialize BLE keyboard
 */
esp_err_t ble_keyboard_init(void)
{
    ESP_LOGI(TAG, "Initializing BLE keyboard host");

    // TODO: Initialize Bluetooth stack
    // TODO: Set up HID over GATT profile
    // TODO: Register callbacks for HID reports

    ESP_LOGW(TAG, "BLE keyboard not yet implemented");

    return ESP_OK;
}

/**
 * Start scanning for BLE keyboards
 */
esp_err_t ble_keyboard_scan(void)
{
    ESP_LOGI(TAG, "Scanning for BLE keyboards");

    // TODO: Start BLE scan for HID devices

    return ESP_OK;
}

/**
 * Pair with BLE keyboard
 */
esp_err_t ble_keyboard_pair(const uint8_t *addr)
{
    ESP_LOGI(TAG, "Pairing with keyboard");

    // TODO: Implement pairing

    connected = true;
    return ESP_OK;
}

/**
 * Check if keyboard is connected
 */
bool ble_keyboard_is_connected(void)
{
    return connected;
}

/**
 * Get last keyboard report
 */
const hid_keyboard_report_t* ble_keyboard_get_report(void)
{
    return &last_report;
}
