/*
 * SSH client implementation
 *
 * TODO: Port libssh or implement custom SSH client with mbedTLS
 * This is a placeholder stub for the POC
 */

#include "ssh_client.h"
#include "terminal.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ssh_client";

static bool connected = false;

/**
 * Initialize SSH client
 */
esp_err_t ssh_client_init(void)
{
    ESP_LOGI(TAG, "SSH client initialized");
    return ESP_OK;
}

/**
 * Connect to SSH server
 */
esp_err_t ssh_client_connect(const ssh_config_t *config)
{
    ESP_LOGI(TAG, "Connecting to %s:%d as %s",
             config->host, config->port, config->username);

    // TODO: Implement actual SSH connection
    // 1. TCP socket connection
    // 2. SSH handshake
    // 3. Key exchange
    // 4. Authentication
    // 5. Request PTY
    // 6. Start shell

    ESP_LOGW(TAG, "SSH client not yet implemented - showing mock connection");

    // Mock success for testing
    terminal_print("  SSH Protocol Version: 2.0\n");
    terminal_print("  Server: MockSSH_1.0\n");
    terminal_print("  Cipher: aes128-ctr\n");
    terminal_print("  MAC: hmac-sha2-256\n");
    terminal_print("\n");
    terminal_print("  Authentication successful!\n");
    terminal_print("\n");
    terminal_print("  Welcome to Cyberdeck Terminal\n");
    terminal_print("  Type 'help' for available commands\n");
    terminal_print("\n");
    terminal_print("  user@host:~$ ");

    connected = true;

    return ESP_OK;
}

/**
 * Disconnect from SSH server
 */
esp_err_t ssh_client_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting from SSH server");

    // TODO: Implement proper disconnect

    connected = false;
    return ESP_OK;
}

/**
 * Send data to SSH server
 */
int ssh_client_send(const uint8_t *data, size_t len)
{
    if (!connected) {
        return -1;
    }

    // TODO: Implement actual SSH data send
    ESP_LOGI(TAG, "Sending %d bytes", len);

    return len;
}

/**
 * Check if connected
 */
bool ssh_client_is_connected(void)
{
    return connected;
}
