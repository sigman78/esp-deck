#include "cyberdeck_app.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ssh_client.h"
#include "storage.h"
#include "wifi_manager.h"

#define CYBERDECK_APP_MAX_PROFILES 8

static const char *TAG = "cyberdeck_app";

static void emit_status(cyberdeck_app_status_fn_t fn,
                        cyberdeck_app_t *app,
                        const char *msg)
{
    if (fn) {
        fn(msg, app->config.user);
    }
}

static const conn_profile_t *resolve_profile(const cyberdeck_app_t *app)
{
    if (app->config.prefer_explicit_connection) {
        return NULL;
    }
    return storage_find_profile(app->profiles, app->profile_count, "default");
}

static void build_ssh_config(cyberdeck_app_t *app, ssh_config_t *out)
{
    memset(out, 0, sizeof(*out));

    out->host     = app->config.default_host;
    out->port     = app->config.default_port;
    out->username = app->config.default_user;
    out->password = app->config.default_password;

    const conn_profile_t *profile = resolve_profile(app);
    if (!profile) {
        return;
    }

    out->host     = profile->host;
    out->port     = profile->port;
    out->username = profile->user;
    out->password = NULL;
    out->private_key = NULL;

    if (profile->auth == STORAGE_AUTH_PASSWORD) {
        out->password = profile->password;
        return;
    }

    snprintf(app->private_key_path, sizeof(app->private_key_path),
             "%s/keys/%s.pem", storage_platform_mount_point(), profile->key_id);
    out->private_key = app->private_key_path;
}

esp_err_t cyberdeck_app_init(cyberdeck_app_t *app,
                             const cyberdeck_app_config_t *config,
                             uint64_t now_ms)
{
    if (!app || !config || !config->default_host || !config->default_user) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(app, 0, sizeof(*app));
    app->config = *config;
    app->state = CYBERDECK_APP_BOOT;
    app->state_deadline_ms = now_ms + app->config.boot_delay_ms;

    if (storage_load_profiles(app->profiles,
                              &app->profile_count,
                              CYBERDECK_APP_MAX_PROFILES) != ESP_OK) {
        ESP_LOGW(TAG, "storage_load_profiles failed; defaults will be used");
        app->profile_count = 0;
    }

    return ESP_OK;
}

void cyberdeck_app_tick(cyberdeck_app_t *app, uint64_t now_ms)
{
    if (!app || app->halted) {
        return;
    }

    switch (app->state) {
    case CYBERDECK_APP_BOOT:
        if (now_ms >= app->state_deadline_ms) {
            app->state = CYBERDECK_APP_WIFI_WAIT;
        }
        break;

    case CYBERDECK_APP_WIFI_WAIT:
        if (!app->wifi_started) {
            emit_status(app->config.status_info, app, "Connecting to WiFi...");
            if (wifi_manager_connect() != ESP_OK) {
                emit_status(app->config.status_fail, app, "WiFi connect failed");
                app->state = CYBERDECK_APP_ERROR;
                app->halted = true;
                break;
            }
            app->wifi_started = true;
            app->state_deadline_ms = now_ms + app->config.wifi_timeout_ms;
        }

        if (wifi_manager_is_connected()) {
            emit_status(app->config.status_ok, app, "WiFi connected");
            app->state = CYBERDECK_APP_SSH_CONNECT;
            app->next_ssh_attempt_ms = now_ms;
        } else if (now_ms >= app->state_deadline_ms) {
            emit_status(app->config.status_fail, app, "WiFi connection timeout");
            app->state = CYBERDECK_APP_ERROR;
            app->halted = true;
        }
        break;

    case CYBERDECK_APP_SSH_CONNECT: {
        if (!wifi_manager_is_connected()) {
            ESP_LOGW(TAG, "WiFi lost, returning to wait state");
            app->wifi_started = false;
            app->state = CYBERDECK_APP_WIFI_WAIT;
            break;
        }

        if (now_ms < app->next_ssh_attempt_ms) {
            break;
        }

        ssh_config_t ssh_cfg;
        build_ssh_config(app, &ssh_cfg);

        emit_status(app->config.status_info, app, "Connecting to SSH server...");
        if (ssh_client_connect(&ssh_cfg) == ESP_OK) {
            emit_status(app->config.status_ok, app, "SSH connected");
            app->state = CYBERDECK_APP_SESSION;
        } else {
            emit_status(app->config.status_fail, app,
                        "SSH connection failed; retrying...");
            app->next_ssh_attempt_ms = now_ms + app->config.ssh_retry_delay_ms;
        }
        break;
    }

    case CYBERDECK_APP_SESSION:
        if (!ssh_client_is_connected()) {
            if (app->config.auto_reconnect) {
                emit_status(app->config.status_fail, app,
                            "SSH session ended; reconnecting...");
                app->state = CYBERDECK_APP_SSH_CONNECT;
                app->next_ssh_attempt_ms = now_ms + app->config.ssh_retry_delay_ms;
            } else {
                emit_status(app->config.status_fail, app, "SSH session ended");
                app->state = CYBERDECK_APP_ERROR;
                app->halted = true;
            }
        }
        break;

    case CYBERDECK_APP_ERROR:
        app->halted = true;
        break;
    }
}

void cyberdeck_app_send_bytes(cyberdeck_app_t *app, const uint8_t *data, size_t len)
{
    (void)app;
    if (!data || len == 0 || !ssh_client_is_connected()) {
        return;
    }
    ssh_client_send(data, len);
}

void cyberdeck_app_handle_pairing_request(cyberdeck_app_t *app)
{
    if (!app) {
        return;
    }
    if (app->config.request_pairing) {
        app->config.request_pairing(app->config.user);
    }
}

cyberdeck_app_state_t cyberdeck_app_state(const cyberdeck_app_t *app)
{
    return app ? app->state : CYBERDECK_APP_ERROR;
}

bool cyberdeck_app_is_running(const cyberdeck_app_t *app)
{
    return app && !app->halted;
}
