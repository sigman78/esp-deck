/*
 * cyberdeck_app.c — the shell.
 *
 * State machine:
 *
 *   BOOT ── delay ──> HOME (profile picker) ──Enter──> CONNECTING ──> SESSION
 *            ^          │  'b'                  ^  ESC     │ ok          │
 *            │          v                       │          v             v
 *            │       PAIRING ──Enter/ESC──> HOME│      HOSTKEY ──trust──>│
 *            │                                  │      (TOFU prompt)     │
 *            └─────── session drop (no auto-reconnect) <────── F12 ──> MENU
 *
 * All shell UI lives in the display overlay layer (app_ui). The vterm cell
 * buffer belongs to the boot splash and the SSH session; the shell never
 * writes ANSI into it except to clear it when a session starts.
 */

#include "cyberdeck_app.h"
#include "app_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ssh_client.h"
#include "storage.h"
#include "vterm.h"
#include "wifi_manager.h"

static const char *TAG = "cyberdeck_app";

/* ---------------------------------------------------------------- state */

typedef enum {
    ST_BOOT = 0,
    ST_HOME,        /* profile picker + status                       */
    ST_PAIRING,     /* BLE keyboard scan list (modal)                */
    ST_HOSTKEY,     /* trust-on-first-use fingerprint prompt (modal) */
    ST_CONNECTING,  /* pending/armed SSH connect                     */
    ST_SESSION,     /* bytes flow to/from SSH                        */
    ST_MENU,        /* in-session overlay menu                       */
} app_state_t;

#define MAX_PROFILES     (8 + 1)          /* stored + synthesized fallback */
#define PAIR_MAX         STORAGE_BLE_MAX
#define PAIR_TIMEOUT_MS  30000
#define PAIR_POLL_MS     250
#define HOME_REFRESH_MS  500
#define TOAST_MS         3000

/* Shell palette — pale green on near-black, classic terminal glow. */
#define UI_FG   RGB565(140, 255, 190)
#define UI_BG   RGB565(0, 24, 16)

static struct {
    cyberdeck_app_config_t cfg;
    app_state_t state;

    conn_profile_t profiles[MAX_PROFILES];
    int  profile_count;
    int  sel;                       /* HOME selection */

    /* HOME list geometry, saved by render_home for touch hit-testing */
    int  home_row0;                 /* screen row of first visible profile */
    int  home_first;                /* index of first visible profile      */
    int  home_visible;              /* visible profile rows                */

    /* connecting */
    int      connect_idx;           /* profile being connected            */
    bool     connect_armed;         /* render one frame, then connect     */
    uint64_t connect_at;            /* not before (auto-reconnect delay)  */
    char     pinned_fp[65];         /* fp to pass as expected_fp, "" = none */

    /* hostkey prompt */
    bool     fp_mismatch;

    /* pairing */
    ble_device_info_t devs[PAIR_MAX];
    int      ndevs;
    int      pair_sel;
    int      pair_row0;             /* screen row of first device entry   */
    int      pair_rowh;             /* rows per device entry (touch size)  */
    uint64_t pair_last_poll;
    uint64_t pair_last_activity;

    /* menu */
    int menu_sel;

    /* toast (SESSION only; UI states draw status inline) */
    char     toast[64];
    uint64_t toast_until;

    uint64_t boot_until;
    uint64_t next_home_refresh;
    bool     halted;
} s;

/* ------------------------------------------------------------ key decode */

typedef enum {
    K_NONE = 0, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_ENTER, K_ESC, K_F12, K_CHAR,
} ui_key_t;

static ui_key_t decode_key(const cyberdeck_input_t *ev, char *ch)
{
    const uint8_t *b = ev->buf;
    int len = ev->len;

    if (len == 1) {
        if (b[0] == 0x1B) return K_ESC;
        if (b[0] == '\r' || b[0] == '\n') return K_ENTER;
        if (b[0] >= 0x20 && b[0] < 0x7F) { if (ch) *ch = (char)b[0]; return K_CHAR; }
        return K_NONE;
    }
    if (len >= 3 && b[0] == 0x1B && (b[1] == '[' || b[1] == 'O')) {
        switch (b[2]) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        }
    }
    if (len == 5 && memcmp(b, "\x1b[24~", 5) == 0) return K_F12;
    return K_NONE;
}

static bool is_f12(const cyberdeck_input_t *ev)
{
    return ev->len == 5 && memcmp(ev->buf, "\x1b[24~", 5) == 0;
}

/* ------------------------------------------------------------- profiles */

static void load_profiles(void)
{
    s.profile_count = 0;
    int n = 0;
    if (storage_load_profiles(s.profiles, &n, MAX_PROFILES - 1) != ESP_OK)
        n = 0;
    s.profile_count = n;

    /* Synthesize "(default)" from the fallback config if present. */
    if (s.cfg.fallback_host && s.cfg.fallback_host[0]) {
        conn_profile_t *p = &s.profiles[s.profile_count++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "(default)");
        snprintf(p->host, sizeof(p->host), "%s", s.cfg.fallback_host);
        p->port = s.cfg.fallback_port ? s.cfg.fallback_port : 22;
        snprintf(p->user, sizeof(p->user), "%s",
                 s.cfg.fallback_user ? s.cfg.fallback_user : "root");
        p->auth = STORAGE_AUTH_PASSWORD;
        snprintf(p->password, sizeof(p->password), "%s",
                 s.cfg.fallback_password ? s.cfg.fallback_password : "");
    }
    if (s.sel >= s.profile_count) s.sel = s.profile_count ? s.profile_count - 1 : 0;
}

static void kick_wifi(void)
{
    wifi_profile_t nets[STORAGE_WIFI_MAX];
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);

    if (n == 0 && s.cfg.fallback_wifi_ssid && s.cfg.fallback_wifi_ssid[0]) {
        memset(&nets[0], 0, sizeof(nets[0]));
        snprintf(nets[0].ssid, sizeof(nets[0].ssid), "%s",
                 s.cfg.fallback_wifi_ssid);
        snprintf(nets[0].password, sizeof(nets[0].password), "%s",
                 s.cfg.fallback_wifi_password ? s.cfg.fallback_wifi_password : "");
        n = 1;
    }

    if (n > 0) {
        wifi_manager_connect(nets, n);
    } else {
        ESP_LOGW(TAG, "no WiFi profiles (wifi.ini empty, no fallback)");
    }
}

/* -------------------------------------------------------------- toasts */

static void toast(uint64_t now, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.toast, sizeof(s.toast), fmt, ap);
    va_end(ap);
    s.toast_until = now + TOAST_MS;
}

/* ------------------------------------------------------------ rendering */

static const char *wifi_status_str(void)
{
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_CONNECTED:  return wifi_manager_get_ip();
    case WIFI_MGR_CONNECTING: return "connecting...";
    case WIFI_MGR_LOST:       return "reconnecting...";
    case WIFI_MGR_FAILED:     return "failed (retrying)";
    default:                  return "off";
    }
}

static const char *ble_status_str(void)
{
    if (!s.cfg.ble || !s.cfg.ble->get_state) return "n/a";
    switch (s.cfg.ble->get_state()) {
    case 4:  return "connected";        /* BLE_CONNECTED       */
    case 3:  return "connecting...";    /* BLE_CONNECTING      */
    case 2:  return "pairing scan";     /* BLE_PAIRING_SCAN    */
    case 1:  return "searching...";     /* BLE_RECONNECT       */
    default: return "idle";
    }
}

static void render_home(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    int w = ui_cols() - 8;
    if (w > 72) w = 72;
    int x = (ui_cols() - w) / 2;
    int h = 10 + (s.profile_count ? s.profile_count : 1);
    if (h > ui_rows() - 2) h = ui_rows() - 2;
    int y = (ui_rows() - h) / 2;

    ui_box(x, y, w, h, " C Y B E R D E C K ");

    ui_printf(x + 2, y + 2, 0, "WiFi %-10s %s",
              wifi_manager_get_ssid()[0] ? wifi_manager_get_ssid() : "-",
              wifi_status_str());
    ui_printf(x + 2, y + 3, 0, "Kbd  %s", ble_status_str());
    ui_hline(x, y + 4, w, UI_BOX_ML, UI_BOX_H, UI_BOX_MR);

    int list_rows = h - 8;
    int first = 0;
    if (s.sel >= list_rows) first = s.sel - list_rows + 1;

    /* Save geometry so touch taps can hit-test a profile row. */
    s.home_row0    = y + 5;
    s.home_first   = first;
    s.home_visible = 0;

    if (s.profile_count == 0) {
        ui_puts(x + 2, y + 5, "no profiles — edit profiles.ini in storage", 0);
    } else {
        for (int i = 0; i < s.profile_count - first && i < list_rows; i++) {
            const conn_profile_t *p = &s.profiles[first + i];
            uint8_t a = (first + i == s.sel) ? OVERLAY_ATTR_INVERSE : 0;
            char line[96];
            snprintf(line, sizeof(line), " %-14s %s@%s:%u %s",
                     p->name, p->user, p->host, (unsigned)p->port,
                     p->auth == STORAGE_AUTH_KEY ? "[key]" : "");
            /* pad to full inner width so the highlight is a solid bar */
            int inner = w - 2;
            int len = (int)strlen(line);
            for (int c = 0; c < inner; c++)
                ui_putch(x + 1 + c, y + 5 + i,
                         c < len ? (uint8_t)line[c] : ' ', a);
            s.home_visible++;
        }
    }

    ui_hline(x, y + h - 3, w, UI_BOX_ML, UI_BOX_H, UI_BOX_MR);
    ui_puts(x + 2, y + h - 2,
            "tap=connect  hold=pair kbd   (or keys: Enter b r w)", 0);

    if (s.toast[0])
        ui_printf(x + 2, y + h - 4, OVERLAY_ATTR_INVERSE, " %s ", s.toast);

    ui_no_cursor();
    ui_present();
}

static void render_pairing(void)
{
    ui_colors(COLOR_BLACK, COLOR_CYAN);
    ui_clear();

    const int rowh  = 2;                    /* 2 cells (32px) per entry — finger-sized */
    int shown = s.ndevs ? s.ndevs : 1;
    int w = 56;
    int x = (ui_cols() - w) / 2;
    int y = 2;
    int h = shown * rowh + 5;               /* title + list + separator + footer + borders */

    ui_box(x, y, w, h, " Pair a keyboard ");

    s.pair_row0 = y + 2;                     /* first device entry row */
    s.pair_rowh = rowh;

    if (s.ndevs == 0) {
        ui_puts(x + 2, y + 2, "Scanning for keyboards...", 0);
    } else {
        for (int i = 0; i < s.ndevs; i++) {
            uint8_t a = (i == s.pair_sel) ? OVERLAY_ATTR_INVERSE : 0;
            int ry = y + 2 + i * rowh;
            /* Full-width bar across the whole entry so the touch target is
             * two rows tall, not one thin line. */
            for (int r = 0; r < rowh; r++)
                for (int c = 1; c < w - 1; c++)
                    ui_putch(x + c, ry + r, ' ', a);
            ui_printf(x + 2, ry, a, "%d. %-.*s", i + 1, w - 8, s.devs[i].name);
        }
    }

    ui_hline(x, y + h - 3, w, UI_BOX_ML, UI_BOX_H, UI_BOX_MR);
    ui_puts(x + 2, y + h - 2, "Tap a keyboard to pair      Esc cancel", 0);
    ui_no_cursor();
    ui_present();
}

static void render_hostkey(void)
{
    ui_colors(s.fp_mismatch ? COLOR_WHITE : UI_FG,
              s.fp_mismatch ? RGB565(96, 0, 0) : UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    const conn_profile_t *p = &s.profiles[s.connect_idx];
    const char *fp = ssh_client_get_fingerprint();

    int w = 72, x = (ui_cols() - w) / 2;
    int h = 10, y = (ui_rows() - h) / 2;

    ui_box(x, y, w, h, s.fp_mismatch ? " ! HOST KEY CHANGED ! "
                                     : " Unknown host ");
    if (s.fp_mismatch) {
        ui_puts(x + 2, y + 2, "The server's key DIFFERS from the pinned one.", 0);
        ui_puts(x + 2, y + 3, "This may be a man-in-the-middle attack.", 0);
    } else {
        ui_printf(x + 2, y + 2, 0, "First connection to %s:%u.",
                  p->host, (unsigned)p->port);
        ui_puts(x + 2, y + 3, "Verify the fingerprint before trusting it.", 0);
    }
    ui_puts(x + 2, y + 5, "SHA256:", 0);
    char half[33];
    memcpy(half, fp, 32); half[32] = '\0';
    ui_puts(x + 10, y + 5, half, 0);
    ui_puts(x + 10, y + 6, fp + 32, 0);

    if (s.fp_mismatch)
        ui_puts(x + 2, y + h - 2,
                "Press Y to REPLACE the pinned key      Esc cancel", 0);
    else
        ui_puts(x + 2, y + h - 2, "Enter trust & connect      Esc cancel", 0);
    ui_no_cursor();
    ui_present();
}

static void render_connecting(const char *msg)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    const conn_profile_t *p = &s.profiles[s.connect_idx];
    int w = 64, x = (ui_cols() - w) / 2;
    int y = ui_rows() / 2 - 2;

    ui_box(x, y, w, 5, " SSH ");
    ui_printf(x + 2, y + 2, 0, "%s %s@%s:%u ...",
              msg, p->user, p->host, (unsigned)p->port);
    ui_no_cursor();
    ui_present();
}

static const char *menu_items[] = {
    "Resume session",
    "Disconnect",
    "Disconnect + profiles",
    "Pair keyboard",
};
#define MENU_COUNT ((int)(sizeof(menu_items) / sizeof(menu_items[0])))

static void render_menu(void)
{
    ui_colors(COLOR_BLACK, COLOR_CYAN);
    ui_clear();   /* transparent outside the box — session stays visible */

    int w = 34, h = MENU_COUNT + 4;
    int x = (ui_cols() - w) / 2;
    int y = (ui_rows() - h) / 2;

    ui_box(x, y, w, h, " Menu ");
    for (int i = 0; i < MENU_COUNT; i++) {
        bool dim = (i == 3 && !s.cfg.ble);   /* no BLE on this platform */
        uint8_t a = (i == s.menu_sel) ? OVERLAY_ATTR_INVERSE : 0;
        char line[40];
        snprintf(line, sizeof(line), " %-28s", menu_items[i]);
        if (dim) line[1] = '(';
        ui_puts(x + 2, y + 2 + i, line, a);
    }
    ui_no_cursor();
    ui_present();
}

static void render_session_toast(uint64_t now)
{
    if (now >= s.toast_until || !s.toast[0]) {
        if (s.state == ST_SESSION) ui_hide();
        return;
    }
    ui_colors(COLOR_BLACK, COLOR_YELLOW);
    ui_clear();
    int len = (int)strlen(s.toast) + 2;
    int x = ui_cols() - len - 1;
    ui_printf(x, 0, 0, " %s ", s.toast);
    ui_present();
}

/* -------------------------------------------------------- state changes */

static void enter_home(uint64_t now)
{
    s.state = ST_HOME;
    s.next_home_refresh = 0;
    (void)now;
    render_home();
}

static void enter_pairing(uint64_t now)
{
    if (!s.cfg.ble || !s.cfg.ble->enter_pairing) return;
    s.cfg.ble->enter_pairing();
    s.state = ST_PAIRING;
    s.ndevs = 0;
    s.pair_sel = 0;
    s.pair_last_poll = 0;
    s.pair_last_activity = now;
    render_pairing();
}

/* Leave pairing: back to the live session if one exists, else HOME. */
static void exit_pairing(uint64_t now)
{
    if (s.cfg.ble && s.cfg.ble->exit_pairing)
        s.cfg.ble->exit_pairing();
    if (ssh_client_is_connected()) {
        s.state = ST_SESSION;
        ui_hide();
    } else {
        enter_home(now);
    }
}

/* Arm a connect to profile idx: one frame of "Connecting", then do it. */
static void start_connect(int idx, uint64_t not_before, uint64_t now)
{
    s.connect_idx   = idx;
    s.connect_at    = not_before;
    s.connect_armed = true;
    s.state         = ST_CONNECTING;

    /* Pinned fingerprint, if we have one for this host. */
    const conn_profile_t *p = &s.profiles[idx];
    if (storage_known_host_get(p->host, p->port,
                               s.pinned_fp, sizeof(s.pinned_fp)) != ESP_OK)
        s.pinned_fp[0] = '\0';

    render_connecting(now < not_before ? "Retrying" : "Connecting to");
}

/* Pin the server's current fingerprint and (re)connect. Called from the
 * hostkey prompt once the user has confirmed — a plain Enter for a first-seen
 * host, but only an explicit 'Y' for a CHANGED key (possible MITM). */
static void hostkey_trust_and_connect(uint64_t now)
{
    const conn_profile_t *p = &s.profiles[s.connect_idx];
    storage_known_host_set(p->host, p->port, ssh_client_get_fingerprint());
    snprintf(s.pinned_fp, sizeof(s.pinned_fp), "%s", ssh_client_get_fingerprint());
    s.connect_armed = true;
    s.connect_at    = now;
    s.state         = ST_CONNECTING;
    render_connecting("Connecting to");
}

static void enter_session(uint64_t now)
{
    s.state = ST_SESSION;
    ui_hide();
    /* Clear whatever the splash/previous session left behind. */
    vterm_write("\x1b[2J\x1b[H", 7);
    toast(now, "connected — F12 for menu");
    render_session_toast(now);
}

static void do_connect(uint64_t now)
{
    static char key_path[160];
    const conn_profile_t *p = &s.profiles[s.connect_idx];

    ssh_config_t cfg = {
        .host        = p->host,
        .port        = p->port,
        .username    = p->user,
        .password    = NULL,
        .private_key = NULL,
        .expected_fp = s.pinned_fp[0] ? s.pinned_fp : NULL,
    };
    if (p->auth == STORAGE_AUTH_KEY) {
        snprintf(key_path, sizeof(key_path), "%s/keys/%s.pem",
                 storage_platform_mount_point(), p->key_id);
        cfg.private_key = key_path;
    } else {
        cfg.password = p->password;
    }

    esp_err_t err = ssh_client_connect(&cfg);

    switch (err) {
    case ESP_OK:
        enter_session(now);
        break;

    case SSH_ERR_HOSTKEY_UNKNOWN:
        s.fp_mismatch = false;
        s.state = ST_HOSTKEY;
        render_hostkey();
        break;

    case SSH_ERR_HOSTKEY_MISMATCH:
        s.fp_mismatch = true;
        s.state = ST_HOSTKEY;
        render_hostkey();
        break;

    case SSH_ERR_AUTH:
        toast(now, "auth failed: %.40s", ssh_client_last_error());
        enter_home(now);
        break;

    default:
        if (s.cfg.auto_reconnect) {
            toast(now, "connect failed — retrying");
            start_connect(s.connect_idx, now + s.cfg.ssh_retry_delay_ms, now);
        } else {
            toast(now, "failed: %.44s", ssh_client_last_error());
            enter_home(now);
        }
        break;
    }
}

/* ---------------------------------------------------------- public API */

esp_err_t cyberdeck_app_init(const cyberdeck_app_config_t *cfg, uint64_t now_ms)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    s.state = ST_BOOT;
    s.boot_until = now_ms + cfg->boot_delay_ms;

    esp_err_t err = ui_init();
    if (err != ESP_OK) return err;

    load_profiles();
    kick_wifi();

    ESP_LOGI(TAG, "shell up: %d profile(s)", s.profile_count);
    return ESP_OK;
}

bool cyberdeck_app_in_session(void)
{
    return s.state == ST_SESSION || s.state == ST_MENU;
}

void cyberdeck_app_tick(uint64_t now)
{
    if (s.halted) return;

    switch (s.state) {
    case ST_BOOT:
        if (now >= s.boot_until)
            enter_home(now);
        break;

    case ST_HOME:
        if (now >= s.next_home_refresh) {
            s.next_home_refresh = now + HOME_REFRESH_MS;
            if (s.toast[0] && now >= s.toast_until) s.toast[0] = '\0';
            render_home();   /* live wifi/ble status */
        }
        break;

    case ST_PAIRING:
        if (now - s.pair_last_activity > PAIR_TIMEOUT_MS) {
            exit_pairing(now);
            break;
        }
        if (now - s.pair_last_poll >= PAIR_POLL_MS && s.cfg.ble) {
            s.pair_last_poll = now;
            ble_device_info_t fresh[PAIR_MAX];
            int n = s.cfg.ble->get_scan_results(fresh, PAIR_MAX);
            if (n != s.ndevs ||
                memcmp(fresh, s.devs, (size_t)n * sizeof(fresh[0])) != 0) {
                memcpy(s.devs, fresh, sizeof(fresh));
                s.ndevs = n;
                if (s.pair_sel >= n) s.pair_sel = n ? n - 1 : 0;
                s.pair_last_activity = now;   /* results still arriving */
                render_pairing();
            }
        }
        break;

    case ST_CONNECTING:
        if (s.connect_armed && now >= s.connect_at) {
            s.connect_armed = false;
            do_connect(now);   /* synchronous; seconds on a bad network */
        }
        break;

    case ST_SESSION:
        if (!ssh_client_is_connected()) {
            if (s.cfg.auto_reconnect) {
                toast(now, "session dropped — reconnecting");
                start_connect(s.connect_idx,
                              now + s.cfg.ssh_retry_delay_ms, now);
            } else {
                toast(now, "session ended");
                enter_home(now);
            }
            break;
        }
        render_session_toast(now);
        break;

    case ST_MENU:
        if (!ssh_client_is_connected()) {
            toast(now, "session ended");
            enter_home(now);
        }
        break;

    default:
        break;
    }
}

void cyberdeck_app_handle_input(const cyberdeck_input_t *ev, uint64_t now)
{
    if (!ev || s.halted) return;

    /* ---- SESSION: forward everything except the menu triggers ---- */
    if (s.state == ST_SESSION) {
        if (ev->type == CYBERDECK_INPUT_LONG_PRESS ||
            (ev->type == CYBERDECK_INPUT_KEY && is_f12(ev))) {
            s.menu_sel = 0;
            s.state = ST_MENU;
            render_menu();
            return;
        }
        if (ev->type == CYBERDECK_INPUT_KEY)
            ssh_client_send(ev->buf, ev->len);
        return;
    }

    /* ---- UI states ---- */
    char ch = 0;
    ui_key_t k = (ev->type == CYBERDECK_INPUT_KEY)
                 ? decode_key(ev, &ch) : K_NONE;

    switch (s.state) {
    case ST_BOOT:
        if (k != K_NONE) enter_home(now);
        break;

    case ST_HOME:
        /* Touch: the only input available before a keyboard is paired.
         * Hold anywhere = open pairing; tap a profile row = connect it. */
        if (ev->type == CYBERDECK_INPUT_LONG_PRESS) {
            if (s.cfg.ble) enter_pairing(now);
            break;
        }
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int row = ev->y / 16;
            int i   = row - s.home_row0;
            if (i >= 0 && i < s.home_visible) {
                s.sel = s.home_first + i;
                if (!wifi_manager_is_connected()) {
                    toast(now, "WiFi not connected yet");
                    render_home();
                } else {
                    start_connect(s.sel, now, now);
                }
            } else {
                render_home();   /* tap outside list: just repaint */
            }
            break;
        }
        switch (k) {
        case K_UP:
            if (s.sel > 0) { s.sel--; render_home(); }
            break;
        case K_DOWN:
            if (s.sel < s.profile_count - 1) { s.sel++; render_home(); }
            break;
        case K_ENTER:
            if (s.profile_count > 0) {
                if (!wifi_manager_is_connected()) {
                    toast(now, "WiFi not connected yet");
                    render_home();
                } else {
                    start_connect(s.sel, now, now);
                }
            }
            break;
        case K_CHAR:
            if (ch == 'b' || ch == 'B') enter_pairing(now);
            else if (ch == 'r' || ch == 'R') { load_profiles(); render_home(); }
            else if (ch == 'w' || ch == 'W') { kick_wifi(); render_home(); }
            break;
        default:
            break;
        }
        break;

    case ST_PAIRING:
        s.pair_last_activity = now;
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int rel = ev->y / 16 - s.pair_row0;
            int idx = (rel >= 0 && s.pair_rowh > 0) ? rel / s.pair_rowh : -1;
            if (idx >= 0 && idx < s.ndevs && s.cfg.ble) {
                s.cfg.ble->select_device(s.devs[idx].addr,
                                         s.devs[idx].addr_type);
                toast(now, "pairing %.32s...", s.devs[idx].name);
                exit_pairing(now);
            } else {
                exit_pairing(now);
            }
            break;
        }
        switch (k) {
        case K_UP:
            if (s.pair_sel > 0) { s.pair_sel--; render_pairing(); }
            break;
        case K_DOWN:
            if (s.pair_sel < s.ndevs - 1) { s.pair_sel++; render_pairing(); }
            break;
        case K_ENTER:
            if (s.ndevs > 0 && s.cfg.ble) {
                s.cfg.ble->select_device(s.devs[s.pair_sel].addr,
                                         s.devs[s.pair_sel].addr_type);
                toast(now, "pairing %.32s...", s.devs[s.pair_sel].name);
                exit_pairing(now);   /* backend continues async */
            }
            break;
        case K_ESC:
            exit_pairing(now);
            break;
        default:
            break;
        }
        break;

    case ST_HOSTKEY:
        if (!s.fp_mismatch) {
            /* First contact (TOFU): a single Enter pins the key and connects. */
            if (k == K_ENTER)     hostkey_trust_and_connect(now);
            else if (k == K_ESC)  enter_home(now);
        } else {
            /* The pinned key CHANGED — possible MITM. Never let a stray Enter
             * silently overwrite a trusted pin; demand an explicit 'Y'. */
            if (k == K_CHAR && (ch == 'y' || ch == 'Y'))
                hostkey_trust_and_connect(now);
            else if (k == K_ESC || k == K_ENTER)
                enter_home(now);
        }
        break;

    case ST_CONNECTING:
        /* Only the armed wait is cancellable; the blocking connect isn't. */
        if (k == K_ESC && s.connect_armed) {
            s.connect_armed = false;
            enter_home(now);
        }
        break;

    case ST_MENU:
        if (k == K_ESC || (ev->type == CYBERDECK_INPUT_KEY && is_f12(ev))) {
            s.state = ST_SESSION;
            ui_hide();
            break;
        }
        switch (k) {
        case K_UP:
            if (s.menu_sel > 0) { s.menu_sel--; render_menu(); }
            break;
        case K_DOWN:
            if (s.menu_sel < MENU_COUNT - 1) { s.menu_sel++; render_menu(); }
            break;
        case K_ENTER:
            switch (s.menu_sel) {
            case 0:   /* resume */
                s.state = ST_SESSION;
                ui_hide();
                break;
            case 1:   /* disconnect (stay on profiles) */
            case 2:   /* disconnect + profiles */
                ssh_client_disconnect();
                enter_home(now);
                break;
            case 3:   /* pair keyboard — session keeps running behind it */
                if (s.cfg.ble)
                    enter_pairing(now);
                break;
            }
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}
