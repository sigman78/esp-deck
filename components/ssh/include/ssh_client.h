/*
 * SSH client interface
 */

#ifndef SSH_CLIENT_H
#define SSH_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Distinct connect failures the shell can react to (beyond ESP_FAIL). */
#define SSH_ERR_BASE              0x7000
#define SSH_ERR_HOSTKEY_UNKNOWN   (SSH_ERR_BASE + 1)  /* no pinned fp, TOFU prompt needed */
#define SSH_ERR_HOSTKEY_MISMATCH  (SSH_ERR_BASE + 2)  /* pinned fp does not match!        */
#define SSH_ERR_AUTH              (SSH_ERR_BASE + 3)  /* all auth methods failed          */

// SSH configuration
typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;
    const char *private_key;  // Optional, for pubkey auth
    /*
     * Pinned host-key fingerprint (lowercase hex SHA256, 64 chars) from a
     * previous session.  NULL = unknown host: the connect stops after the
     * handshake with SSH_ERR_HOSTKEY_UNKNOWN so the caller can show a
     * trust-on-first-use prompt, then retry with the fingerprint set.
     */
    const char *expected_fp;
} ssh_config_t;

/**
 * Initialize SSH client
 */
esp_err_t ssh_client_init(void);

/**
 * Connect to SSH server (TCP, handshake, host-key check, auth, PTY, shell).
 *
 * @return ESP_OK, SSH_ERR_HOSTKEY_UNKNOWN, SSH_ERR_HOSTKEY_MISMATCH,
 *         SSH_ERR_AUTH, or ESP_FAIL.
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

/**
 * SHA256 host-key fingerprint (64 hex chars) observed during the most
 * recent connect attempt — valid after ESP_OK, SSH_ERR_HOSTKEY_UNKNOWN
 * and SSH_ERR_HOSTKEY_MISMATCH. Empty string otherwise.
 */
const char *ssh_client_get_fingerprint(void);

/**
 * Human-readable description of the most recent connect failure.
 */
const char *ssh_client_last_error(void);

#endif // SSH_CLIENT_H
