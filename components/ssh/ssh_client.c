/*
 * ssh_client.c — libssh2-based SSH client for the Cyberdeck terminal.
 *
 * Lifecycle:
 *   ssh_client_connect()  — TCP connect, SSH handshake, auth, PTY, shell
 *   ssh_client_send()     — write bytes to the remote shell
 *   ssh_client_is_connected() — poll connection state
 *   ssh_client_disconnect() — clean shutdown
 *
 * A dedicated FreeRTOS task (ssh_read_task, core 0) blocks on
 * libssh2_channel_read() and feeds output into vterm.  The session is
 * switched to non-blocking mode after the shell is opened so that the
 * read task can detect EOF/errors promptly.
 */

#include "ssh_client.h"
#include "vterm.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "libssh2.h"

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "ssh_client";

/* -------------------------------------------------------------------------
 * Custom libssh2 allocators — prefer SPIRAM for session/channel/packet
 * buffers so that fragmented internal DRAM is not exhausted by large SSH
 * receive bursts.  Falls back to internal DRAM when SPIRAM is unavailable.
 *
 * s_alloc_bytes tracks live bytes held by libssh2 at any moment.
 * heap_caps_get_allocated_size() returns the actual block size so the
 * counter reflects real heap consumption including allocator overhead.
 * ---------------------------------------------------------------------- */
static volatile size_t s_alloc_bytes = 0;

static void *ssh_malloc(size_t size, void **abstract)
{
    (void)abstract;
    void *p = heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (p) s_alloc_bytes += heap_caps_get_allocated_size(p);
    return p;
}

static void *ssh_realloc(void *ptr, size_t size, void **abstract)
{
    (void)abstract;
    size_t old_size = ptr ? heap_caps_get_allocated_size(ptr) : 0;
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    if (p) {
        s_alloc_bytes -= old_size;
        s_alloc_bytes += heap_caps_get_allocated_size(p);
    }
    /* On failure ptr is still valid; its size remains counted — no change. */
    return p;
}

static void ssh_free(void *ptr, void **abstract)
{
    (void)abstract;
    if (ptr) {
        s_alloc_bytes -= heap_caps_get_allocated_size(ptr);
        heap_caps_free(ptr);
    }
}

/* Password stashed during connection for the kbd-interactive callback. */
static const char *s_kb_password = NULL;

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */
static LIBSSH2_SESSION  *s_session    = NULL;
static LIBSSH2_CHANNEL  *s_channel    = NULL;
static int               s_sock       = -1;
static TaskHandle_t      s_read_task  = NULL;
static volatile bool     s_connected  = false;
static bool              s_libssh2_initialized = false;

/* -------------------------------------------------------------------------
 * ssh_cleanup — release all libssh2 and socket resources
 * ---------------------------------------------------------------------- */
static void ssh_cleanup(void)
{
    if (s_channel) {
        libssh2_channel_close(s_channel);
        libssh2_channel_free(s_channel);
        s_channel = NULL;
    }
    if (s_session) {
        libssh2_session_disconnect(s_session, "bye");
        libssh2_session_free(s_session);
        s_session = NULL;
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_connected = false;
    ESP_LOGI(TAG, "SSH cleanup done  — libssh2 heap: %zu B (expect 0)", s_alloc_bytes);
}

/* -------------------------------------------------------------------------
 * vterm response callback — sends terminal responses (e.g. DA1, cursor
 * position reports) from libtsm back to the remote over SSH.
 * ---------------------------------------------------------------------- */
static void ssh_vterm_response_cb(const char *data, size_t len, void *user)
{
    (void)user;
    if (s_connected && s_channel && len > 0) {
        /* Use the blocking send path; responses are tiny. */
        libssh2_session_set_blocking(s_session, 1);
        libssh2_channel_write(s_channel, data, len);
        libssh2_session_set_blocking(s_session, 0);
    }
}

/* -------------------------------------------------------------------------
 * Log the last libssh2 error string for the current session.
 * ---------------------------------------------------------------------- */
static void log_last_error(const char *context)
{
    char *errmsg = NULL;
    libssh2_session_last_error(s_session, &errmsg, NULL, 0);
    ESP_LOGE(TAG, "%s: %s", context, errmsg ? errmsg : "unknown");
}

/* -------------------------------------------------------------------------
 * ssh_read_task — pinned to core 0, feeds remote output into vterm.
 *
 * The task MUST call vTaskDelay() on every iteration — not just on EAGAIN.
 * libssh2_channel_read() can return data continuously (e.g. streaming ASCII
 * art) and never block.  Without a yield, IDLE0 is starved and the task
 * watchdog fires.  One tick per iteration caps throughput at ~51 KB/s which
 * is far above any terminal's practical limit.
 *
 * Keepalive packets are sent via libssh2_keepalive_send() which internally
 * tracks timing; we call it on every EAGAIN idle cycle.
 * ---------------------------------------------------------------------- */
static void ssh_read_task(void *arg)
{
    char buf[512];
    TickType_t last_stat = xTaskGetTickCount();

    while (s_connected) {
        int n = libssh2_channel_read(s_channel, buf, sizeof(buf));
        if (n > 0) {
            vterm_write(buf, (size_t)n);
            vterm_flush();
        } else if (n == LIBSSH2_ERROR_EAGAIN) {
            /* No data — good time to send a keepalive if one is due. */
#if CONFIG_SSH_KEEPALIVE_INTERVAL > 0
            int next_ka = 0;
            libssh2_keepalive_send(s_session, &next_ka);
#endif
        } else {
            /* EOF or unrecoverable error */
            log_last_error("channel_read");
            s_connected = false;
            break;
        }

        if (libssh2_channel_eof(s_channel)) {
            ESP_LOGI(TAG, "ssh_read_task: channel EOF");
            s_connected = false;
            break;
        }

        /* Periodic memory stat — once every 10 s. */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stat) >= pdMS_TO_TICKS(30000)) {
            ESP_LOGI(TAG, "libssh2 heap: %zu B", s_alloc_bytes);
            last_stat = now;
        }

        /* Yield every iteration so IDLE0 can run and reset the task WDT.
         * vTaskDelay(1) blocks for exactly one FreeRTOS tick regardless of
         * tick rate; use pdMS_TO_TICKS(10) on EAGAIN for a longer sleep. */
        vTaskDelay(n > 0 ? 1 : pdMS_TO_TICKS(10));
    }

    s_read_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Keyboard-interactive response callback.
 * Responds to every prompt with s_kb_password (set before auth attempt).
 * libssh2 frees responses[i].text after the callback returns.
 * ---------------------------------------------------------------------- */
static LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(kbd_callback)
{
    (void)name; (void)name_len;
    (void)instruction; (void)instruction_len;
    (void)abstract;
    const char *pwd = s_kb_password ? s_kb_password : "";
    for (int i = 0; i < num_prompts; i++) {
        responses[i].text   = strdup(pwd);
        responses[i].length = (unsigned int)strlen(pwd);
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

esp_err_t ssh_client_init(void)
{
    /* libssh2_init() is deferred to first connect; nothing to do here. */
    return ESP_OK;
}

esp_err_t ssh_client_connect(const ssh_config_t *config)
{
    if (!config || !config->host || !config->username) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    /* ── 0. Release any leftover state from a previous session ──────── *
     * ssh_read_task sets s_connected=false on disconnect but does not    *
     * call ssh_cleanup() — that must happen here before we allocate new  *
     * libssh2 objects, otherwise session_init() fails with OOM.         */
    ssh_cleanup();

    /* ── 1. TCP connect ─────────────────────────────────────────────── */
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", (int)config->port);

    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    ESP_LOGI(TAG, "Resolving %s:%s", config->host, port_str);
    if (getaddrinfo(config->host, port_str, &hints, &res) != 0 || res == NULL) {
        ESP_LOGE(TAG, "getaddrinfo failed for %s", config->host);
        return ESP_FAIL;
    }

    s_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        freeaddrinfo(res);
        return ESP_FAIL;
    }

    int tcpFlag = 1;
    setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY, &tcpFlag, sizeof(int));

    ESP_LOGI(TAG, "Connecting TCP socket...");
    if (connect(s_sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "TCP connect failed");
        freeaddrinfo(res);
        ssh_cleanup();
        return ESP_FAIL;
    }
    freeaddrinfo(res);
    ESP_LOGI(TAG, "TCP connected");

    /* ── 2. libssh2 init (once) ─────────────────────────────────────── */
    if (!s_libssh2_initialized) {
        if (libssh2_init(0) != 0) {
            ESP_LOGE(TAG, "libssh2_init failed");
            ssh_cleanup();
            return ESP_FAIL;
        }
        s_libssh2_initialized = true;
    }

    /* ── 3. Session init (custom SPIRAM-preferring allocators) ─────── */
    s_session = libssh2_session_init_ex(ssh_malloc, ssh_free, ssh_realloc, NULL);
    if (!s_session) {
        ESP_LOGE(TAG, "libssh2_session_init_ex failed");
        ssh_cleanup();
        return ESP_FAIL;
    }
    libssh2_session_set_blocking(s_session, 1);

    /* ── 4. SSH handshake ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "SSH handshake...");
    int rc = libssh2_session_handshake(s_session, s_sock);
    if (rc != 0) {
        ESP_LOGE(TAG, "SSH handshake failed: %d", rc);
        log_last_error("handshake");
        ssh_cleanup();
        return ESP_FAIL;
    }

    /* ── 4b. Keepalive configuration ───────────────────────────────── */
#if CONFIG_SSH_KEEPALIVE_INTERVAL > 0
    /* want_reply=0: we send traffic to keep NAT alive but don't ask the
     * server to respond.  want_reply=1 causes the server to send back
     * SSH_MSG_REQUEST_SUCCESS which libssh2 must allocate a buffer for;
     * under heap pressure that allocation fails with LIBSSH2_ERROR_ALLOC
     * and drops the channel.                                              */
    libssh2_keepalive_config(s_session, 0 /* want_reply */,
                             CONFIG_SSH_KEEPALIVE_INTERVAL);
    ESP_LOGI(TAG, "Keepalive every %d s", CONFIG_SSH_KEEPALIVE_INTERVAL);
#endif

    /* ── 5. Log host fingerprint ────────────────────────────────────── */
    const char *fp = libssh2_hostkey_hash(s_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (fp) {
        char fp_hex[65] = {0};
        for (int i = 0; i < 32; i++)
            snprintf(fp_hex + i * 2, 3, "%02x", (unsigned char)fp[i]);
        ESP_LOGI(TAG, "Host fingerprint SHA256: %s", fp_hex);
    }

    /* ── 6. Authentication ──────────────────────────────────────────── */
    ESP_LOGI(TAG, "Authenticating as '%s'...", config->username);

    /* Query which methods the server accepts. */
    char *authlist = libssh2_userauth_list(s_session, config->username,
                                           (unsigned int)strlen(config->username));
    if (!authlist) {
        if (libssh2_userauth_authenticated(s_session)) {
            ESP_LOGI(TAG, "Server requires no authentication");
            goto auth_done;
        }
        log_last_error("userauth_list");
        ssh_cleanup();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Server auth methods: %s", authlist);

    s_kb_password = config->password;  /* stash for kbd-interactive callback */
    rc = LIBSSH2_ERROR_AUTHENTICATION_FAILED;

    /* Try public-key first if a key path was provided. */
    if (config->private_key && strstr(authlist, "publickey")) {
        rc = libssh2_userauth_publickey_fromfile(s_session, config->username,
                                                 NULL, config->private_key, NULL);
        if (rc == 0) goto auth_done;
        ESP_LOGW(TAG, "Pubkey auth failed (%d)", rc);
    }

    /* Try plain password. */
    if (strstr(authlist, "password")) {
        rc = libssh2_userauth_password(s_session, config->username,
                                       config->password ? config->password : "");
        if (rc == 0) goto auth_done;
        ESP_LOGW(TAG, "Password auth failed (%d)", rc);
    }

    /* Try keyboard-interactive (responds every prompt with the password). */
    if (strstr(authlist, "keyboard-interactive")) {
        rc = libssh2_userauth_keyboard_interactive(s_session, config->username,
                                                   kbd_callback);
        if (rc == 0) goto auth_done;
        ESP_LOGW(TAG, "KBD-interactive auth failed (%d)", rc);
    }

    s_kb_password = NULL;
    log_last_error("authentication");
    ssh_cleanup();
    return ESP_FAIL;

auth_done:
    s_kb_password = NULL;
    ESP_LOGI(TAG, "Authenticated");

    /* ── 7. Open shell channel ──────────────────────────────────────── */
    s_channel = libssh2_channel_open_ex(s_session,
                                        "session", sizeof("session") - 1,
                                        CONFIG_SSH_RECV_WINDOW,
                                        LIBSSH2_CHANNEL_PACKET_DEFAULT,
                                        NULL, 0);
    if (!s_channel) {
        ESP_LOGE(TAG, "Channel open failed");
        ssh_cleanup();
        return ESP_FAIL;
    }

    /* ── 8. Request PTY ─────────────────────────────────────────────── */
    rc = libssh2_channel_request_pty_ex(s_channel,
                                        "xterm-256color", 14,
                                        NULL, 0,
                                        CONFIG_TERMINAL_WIDTH,
                                        CONFIG_TERMINAL_HEIGHT,
                                        0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "PTY request failed: %d", rc);
        ssh_cleanup();
        return ESP_FAIL;
    }

    /* ── 9. Start shell ─────────────────────────────────────────────── */
    rc = libssh2_channel_shell(s_channel);
    if (rc != 0) {
        ESP_LOGE(TAG, "Shell request failed: %d", rc);
        ssh_cleanup();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Shell opened");

    /* ── 10. Switch to non-blocking and wire vterm responses ────────── */
    libssh2_session_set_blocking(s_session, 0);
    vterm_set_response_cb(ssh_vterm_response_cb, NULL);

    /* ── 11. Spawn read task on core 0 ──────────────────────────────── */
    s_connected = true;
    BaseType_t ret = xTaskCreatePinnedToCore(ssh_read_task, "ssh_read",
                                             8192, NULL, 6,
                                             &s_read_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ssh_read_task");
        s_connected = false;
        ssh_cleanup();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SSH session ready — libssh2 heap: %zu B", s_alloc_bytes);
    return ESP_OK;
}

esp_err_t ssh_client_disconnect(void)
{
    s_connected = false;

    /* Give the read task a chance to notice and exit cleanly. */
    if (s_read_task) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s_read_task) {
            vTaskDelete(s_read_task);
            s_read_task = NULL;
        }
    }

    vterm_set_response_cb(NULL, NULL);
    ssh_cleanup();
    return ESP_OK;
}

int ssh_client_send(const uint8_t *data, size_t len)
{
    if (!s_connected || !s_channel || len == 0)
        return -1;

    ssize_t sent = 0;
    while ((size_t)sent < len) {
        ssize_t rc = libssh2_channel_write(s_channel,
                                           (const char *)data + sent,
                                           len - (size_t)sent);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (rc < 0) {
            ESP_LOGW(TAG, "channel_write error: %d", (int)rc);
            return -1;
        }
        sent += rc;
    }
    return (int)sent;
}

bool ssh_client_is_connected(void)
{
    return s_connected;
}
