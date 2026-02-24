/*
 * SSH client interface
 */

#ifndef SSH_CLIENT_H
#define SSH_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// SSH configuration
typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    const char *private_key;  // Optional, for pubkey auth
} ssh_config_t;

/**
 * Initialize SSH client
 */
esp_err_t ssh_client_init(void);

/**
 * Connect to SSH server
 *
 * @param config SSH connection configuration
 * @return ESP_OK on success
 */
esp_err_t ssh_client_connect(const ssh_config_t *config);

/**
 * Disconnect from SSH server
 */
esp_err_t ssh_client_disconnect(void);

/**
 * Send data to SSH server
 *
 * @param data Data to send
 * @param len Data length
 * @return Number of bytes sent, or -1 on error
 */
int ssh_client_send(const uint8_t *data, size_t len);

/**
 * Check if connected
 *
 * @return true if connected
 */
bool ssh_client_is_connected(void);

#endif // SSH_CLIENT_H
