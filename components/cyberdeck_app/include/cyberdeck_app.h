#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage.h"

typedef enum {
    CYBERDECK_APP_BOOT = 0,
    CYBERDECK_APP_WIFI_WAIT,
    CYBERDECK_APP_SSH_CONNECT,
    CYBERDECK_APP_SESSION,
    CYBERDECK_APP_ERROR,
} cyberdeck_app_state_t;

typedef void (*cyberdeck_app_status_fn_t)(const char *msg, void *user);
typedef void (*cyberdeck_app_action_fn_t)(void *user);

typedef struct {
    uint64_t boot_delay_ms;
    uint64_t wifi_timeout_ms;
    uint64_t ssh_retry_delay_ms;
    bool auto_reconnect;
    bool prefer_explicit_connection;

    const char *default_host;
    uint16_t    default_port;
    const char *default_user;
    const char *default_password;

    cyberdeck_app_status_fn_t status_info;
    cyberdeck_app_status_fn_t status_ok;
    cyberdeck_app_status_fn_t status_fail;
    cyberdeck_app_action_fn_t request_pairing;
    void                     *user;
} cyberdeck_app_config_t;

typedef struct cyberdeck_app_s {
    cyberdeck_app_config_t config;
    cyberdeck_app_state_t  state;
    bool                   wifi_started;
    uint64_t               state_deadline_ms;
    uint64_t               next_ssh_attempt_ms;
    bool                   halted;

    conn_profile_t profiles[8];
    int  profile_count;
    char private_key_path[160];
} cyberdeck_app_t;

esp_err_t cyberdeck_app_init(cyberdeck_app_t *app,
                             const cyberdeck_app_config_t *config,
                             uint64_t now_ms);

void cyberdeck_app_tick(cyberdeck_app_t *app, uint64_t now_ms);
void cyberdeck_app_send_bytes(cyberdeck_app_t *app, const uint8_t *data, size_t len);
void cyberdeck_app_handle_pairing_request(cyberdeck_app_t *app);

cyberdeck_app_state_t cyberdeck_app_state(const cyberdeck_app_t *app);
bool cyberdeck_app_is_running(const cyberdeck_app_t *app);
