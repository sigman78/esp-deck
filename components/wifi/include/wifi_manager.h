/*
 * WiFi manager interface
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize WiFi manager
 */
esp_err_t wifi_manager_init(void);

/**
 * Connect to WiFi network
 */
esp_err_t wifi_manager_connect(void);

/**
 * Disconnect from WiFi
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * Check if connected to WiFi
 */
bool wifi_manager_is_connected(void);

/**
 * Get IP address
 */
const char* wifi_manager_get_ip(void);

#endif // WIFI_MANAGER_H
