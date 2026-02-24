/*
 * BLE keyboard (HID over GATT) interface
 */

#ifndef BLE_KEYBOARD_H
#define BLE_KEYBOARD_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// HID keyboard report
typedef struct {
    uint8_t modifiers;  // Ctrl, Shift, Alt, GUI
    uint8_t reserved;
    uint8_t keys[6];    // Up to 6 simultaneous keys
} hid_keyboard_report_t;

/**
 * Initialize BLE keyboard (HID host)
 */
esp_err_t ble_keyboard_init(void);

/**
 * Start scanning for BLE keyboards
 */
esp_err_t ble_keyboard_scan(void);

/**
 * Pair with BLE keyboard
 *
 * @param addr BLE device address
 */
esp_err_t ble_keyboard_pair(const uint8_t *addr);

/**
 * Check if keyboard is connected
 */
bool ble_keyboard_is_connected(void);

/**
 * Get last keyboard report
 */
const hid_keyboard_report_t* ble_keyboard_get_report(void);

#endif // BLE_KEYBOARD_H
