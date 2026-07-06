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

#ifndef BUILD_SIMULATOR
#include "esp_heap_caps.h"   /* free-RAM stats in the header */
#endif

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
#define ANIM_PERIOD_MS   100          /* ~10 fps subtle UI animation */
#define TOAST_MS         3000

/* Shell palette — VGA phosphor green on black. Per-cell accents (OVERLAY_COL_*)
 * layer the rest of the classic 16-color set on top. */
#define UI_FG   RGB565(85, 255, 85)   /* VGA bright green */
#define UI_BG   RGB565(0, 0, 0)       /* VGA black        */

/* A page of finger-sized tiles laid out in a grid, with two-axis touch
 * hit-testing. Recomputed by each render_*() and saved for the tap handler.
 * All dimensions are in 8x16-px character cells. */
typedef struct {
    int x0, y0;        /* top-left cell of the grid            */
    int tw, th;        /* tile size in cells                   */
    int gx, gy;        /* gutter between tiles, in cells       */
    int ncols, nrows;  /* tiles per page                       */
    int count;         /* live tiles on this page (<= ncols*nrows) */
} tilegrid_t;

static struct {
    cyberdeck_app_config_t cfg;
    app_state_t state;

    conn_profile_t profiles[MAX_PROFILES];
    int  profile_count;
    int  sel;                       /* HOME tile selection */

    /* Tile grid of the current screen, saved for touch hit-testing. */
    tilegrid_t grid;

    /* connecting */
    int      connect_idx;           /* profile being connected            */
    bool     connect_armed;         /* render one frame, then connect     */
    uint64_t connect_at;            /* not before (auto-reconnect delay)  */
    char     pinned_fp[65];         /* fp to pass as expected_fp, "" = none */

    /* hostkey prompt */
    bool     fp_mismatch;
    bool     hostkey_armed;         /* mismatch REPLACE needs a 2nd tap   */

    /* pairing */
    ble_device_info_t devs[PAIR_MAX];
    int      ndevs;
    int      pair_sel;
    uint64_t pair_last_poll;
    uint64_t pair_last_activity;

    /* menu */
    int menu_sel;

    /* toast (SESSION only; UI states draw status inline) */
    char     toast[64];
    uint64_t toast_until;

    uint64_t boot_until;
    uint64_t next_home_refresh;
    uint32_t anim_frame;            /* advances ~10 fps for subtle animation */
    uint64_t next_anim;             /* next animated re-render (PAIRING)      */
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

/* ---------------------------------------------------------- tile grid */

/* Cell coordinates of tile @p slot's top-left corner. */
static int tile_x(const tilegrid_t *g, int slot)
{
    return g->x0 + (slot % g->ncols) * (g->tw + g->gx);
}
static int tile_y(const tilegrid_t *g, int slot)
{
    return g->y0 + (slot / g->ncols) * (g->th + g->gy);
}

/* Map a touch pixel to a tile slot, or -1 for a gutter / margin / empty cell.
 * This is the two-axis hit-test: a tap must land inside a tile on BOTH axes,
 * not merely on the right row. */
static int tile_hit(const tilegrid_t *g, int px, int py)
{
    if (g->ncols <= 0 || g->nrows <= 0) return -1;
    int cc = px / 8  - g->x0;
    int cr = py / 16 - g->y0;
    if (cc < 0 || cr < 0) return -1;
    int pitchx = g->tw + g->gx, pitchy = g->th + g->gy;
    int col = cc / pitchx, row = cr / pitchy;
    if (col >= g->ncols || row >= g->nrows) return -1;
    if (cc % pitchx >= g->tw || cr % pitchy >= g->th) return -1;  /* gutter */
    int slot = row * g->ncols + col;
    return slot < g->count ? slot : -1;
}

/* Keyboard navigation within the grid (arrow keys). */
static int tile_nav(const tilegrid_t *g, int sel, ui_key_t k)
{
    if (g->count <= 0) return 0;
    int col = sel % g->ncols, row = sel / g->ncols;
    switch (k) {
    case K_LEFT:  if (col > 0)                                sel -= 1;       break;
    case K_RIGHT: if (col < g->ncols - 1 && sel + 1 < g->count) sel += 1;     break;
    case K_UP:    if (row > 0)                                sel -= g->ncols; break;
    case K_DOWN:  if (sel + g->ncols < g->count)              sel += g->ncols; break;
    default: break;
    }
    if (sel < 0)             sel = 0;
    if (sel >= g->count)     sel = g->count - 1;
    return sel;
}

/* ------------------------------------------------------------- profiles */

static void load_profiles(void)
{
    s.profile_count = 0;
    int n = 0;
    if (storage_load_profiles(s.profiles, &n, MAX_PROFILES - 1) != ESP_OK)
        n = 0;
    s.profile_count = n;

    /* Synthesize "(default)" from the Kconfig fallback ONLY when profiles.ini
     * gave us nothing — otherwise a populated file gets padded with a
     * redundant extra entry. */
    if (n == 0 && s.cfg.fallback_host && s.cfg.fallback_host[0]) {
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

/* Shared full-screen picker grid (HOME + PAIRING): 3 x 4 tiles, each 30 x 5
 * cells (240 x 80 px ~ 15 mm tall — comfortably above a finger target). */
static tilegrid_t picker_grid(int count)
{
    tilegrid_t g = { .x0 = 3, .y0 = 4, .tw = 30, .th = 5,
                     .gx = 2, .gy = 1, .ncols = 3, .nrows = 4 };
    int cap = g.ncols * g.nrows;
    g.count = count < cap ? count : cap;
    return g;
}

/* Full-width double rule (═══…) on a row. Special glyphs must go through
 * ui_putch — ui_puts/ui_printf only emit Latin-1 bytes. */
static void draw_rule(int row)
{
    for (int i = 0; i < ui_cols(); i++) ui_putch(i, row, UI_DH, 0);
}

/* Animated rule: a cyan ░▒▓█ "comet" sweeps left→right along the divider. */
static void draw_rule_scan(int row, uint32_t frame)
{
    int W = ui_cols();
    draw_rule(row);
    static const uint16_t comet[4] = { UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK };
    int head = (int)((frame * 2u) % (uint32_t)W);   /* 2 cells/frame */
    ui_pen(OVERLAY_COL_CYAN);
    for (int k = 0; k < 4; k++) {
        int x = head - (3 - k);
        if (x >= 0 && x < W) ui_putch(x, row, comet[k], 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* 8-frame braille spinner (U+2800 block — present in the font). */
static uint16_t spinner_glyph(uint32_t frame)
{
    static const uint16_t sp[8] = {
        0x280B, 0x2819, 0x2839, 0x2838, 0x283C, 0x2834, 0x2826, 0x2827
    };
    return sp[frame % 8];
}

/* Title chip framed by a shade gradient: ░▒▓█ TEXT █▓▒░, drawn at cell x0 on
 * row 0. The flanking blocks glow: a cyan "spark" travels through them each
 * frame for a subtle animated shimmer. Its total width is strlen(text)+10. */
static void draw_titlebar(int x0, const char *text, uint32_t frame)
{
    int spark = (int)((frame / 3u) % 4u);
    int x = x0;
    static const uint16_t lg[4] = { UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK };
    for (int i = 0; i < 4; i++) {
        ui_pen(i == spark ? OVERLAY_COL_CYAN : OVERLAY_COL_MAGENTA);
        ui_putch(x++, 0, lg[i], 0);
    }
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(x++, 0, ' ', OVERLAY_ATTR_INVERSE);
    ui_puts (x, 0, text, OVERLAY_ATTR_INVERSE);
    x += (int)strlen(text);
    ui_putch(x++, 0, ' ', OVERLAY_ATTR_INVERSE);
    static const uint16_t rg[4] = { UI_BLOCK, UI_SHADE3, UI_SHADE2, UI_SHADE1 };
    for (int i = 0; i < 4; i++) {
        ui_pen((3 - i) == spark ? OVERLAY_COL_CYAN : OVERLAY_COL_MAGENTA);
        ui_putch(x++, 0, rg[i], 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* Free-RAM summary for the header. */
static void ram_stats(char *buf, size_t sz)
{
#ifdef BUILD_SIMULATOR
    snprintf(buf, sz, "host build");
#else
    unsigned in = (unsigned)(heap_caps_get_free_size(
                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    unsigned ps = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    snprintf(buf, sz, "int %uK  psram %uK", in, ps);
#endif
}

/* Little ● / ○ LED then a label + value, cyberpunk status line. */
static void draw_status_led(int row, bool on, const char *label, const char *value)
{
    ui_pen(on ? OVERLAY_COL_GREEN : OVERLAY_COL_RED);
    ui_putch(2, row, on ? UI_LED_ON : UI_LED_OFF, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_printf(4, row, 0, "%-4s %s", label, value);
}

/* 5x5 block glyphs for the boot logo (row-major, '#' = filled). */
static const char *boot_glyph(char c)
{
    switch (c) {
    case 'C': return "#####" "#    " "#    " "#    " "#####";
    case 'Y': return "#   #" " # # " "  #  " "  #  " "  #  ";
    case 'B': return "#### " "#   #" "#### " "#   #" "#### ";
    case 'E': return "#####" "#    " "###  " "#    " "#####";
    case 'R': return "#### " "#   #" "#### " "#  # " "#   #";
    case 'D': return "#### " "#   #" "#   #" "#   #" "#### ";
    case 'K': return "#   #" "#  # " "###  " "#  # " "#   #";
    case '*': return "  #  " "# # #" " ### " "# # #" "  #  ";
    default:  return "     " "     " "     " "     " "     ";
    }
}

/* Boot splash: a big CYBER*DECK block logo that wipes in left→right over ~80%
 * of the boot delay (a bright white scan edge leads the reveal), then holds.
 * Runs from the ST_BOOT tick while WiFi/BLE come up in the background. */
static void render_boot(uint64_t now)
{
    static const char LOGO[] = "CYBER*DECK";
    const int GW = 5, GH = 5, GAP = 1;
    int n = (int)strlen(LOGO);
    int total_w = n * (GW + GAP) - GAP;
    int x0 = (ui_cols() - total_w) / 2;
    int y0 = ui_rows() / 4;

    uint64_t start     = s.boot_until - s.cfg.boot_delay_ms;
    uint32_t reveal_ms = s.cfg.boot_delay_ms * 4 / 5;
    uint32_t el        = (uint32_t)(now - start);
    int reveal = reveal_ms ? (int)((uint64_t)el * total_w / reveal_ms) : total_w;
    if (reveal > total_w) reveal = total_w;

    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    for (int i = 0; i < n; i++) {
        const char *g = boot_glyph(LOGO[i]);
        int gx = x0 + i * (GW + GAP);
        uint8_t base = (LOGO[i] == '*') ? OVERLAY_COL_MAGENTA : OVERLAY_COL_CYAN;
        for (int r = 0; r < GH; r++)
            for (int c = 0; c < GW; c++) {
                int col_abs = gx + c - x0;
                if (col_abs >= reveal || g[r * GW + c] != '#') continue;
                bool edge = (col_abs >= reveal - 2);   /* glowing scan front */
                ui_pen(edge ? OVERLAY_COL_WHITE : base);
                ui_putch(gx + c, y0 + r, UI_BLOCK, 0);
            }
    }

    ui_pen(OVERLAY_COL_GREEN);
    char sub[24];
    snprintf(sub, sizeof(sub), "INITIALIZING%.*s", (int)(s.anim_frame % 4), "...");
    ui_puts((ui_cols() - 15) / 2, y0 + GH + 2, sub, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    ui_no_cursor();
    ui_present();
}

static void render_home(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    /* ── Status HUD on the LEFT (rows 0-2) ─────────────────────────── */
    char net[48];
    snprintf(net, sizeof(net), "%-16s %s",
             wifi_manager_get_ssid()[0] ? wifi_manager_get_ssid() : "-",
             wifi_status_str());
    draw_status_led(0, wifi_manager_is_connected(), "NET", net);

    bool kbd = s.cfg.ble && s.cfg.ble->get_state && s.cfg.ble->get_state() == 4;
    const char *kn = (kbd && s.cfg.ble->get_name) ? s.cfg.ble->get_name() : "";
    char kbdinfo[64];
    snprintf(kbdinfo, sizeof(kbdinfo), "%-11s %s", ble_status_str(), kn);
    draw_status_led(1, kbd, "KBD", kbdinfo);

    char ram[48];
    ram_stats(ram, sizeof(ram));
    ui_pen(OVERLAY_COL_BLUE);
    ui_putch(2, 2, UI_DIAMOND, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_printf(4, 2, 0, "RAM  %s", ram);

    /* ── Title + version on the RIGHT (rows 0-1) ────────────────────── */
    int tw = (int)strlen("CYBERDECK") + 10;
    draw_titlebar(ui_cols() - tw - 1, "CYBERDECK", s.anim_frame);
    char ver[24];
    snprintf(ver, sizeof(ver), "// %s", s.cfg.version ? s.cfg.version : "?");
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - (int)strlen(ver) - 1, 1, ver, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_rule_scan(3, s.anim_frame);

    /* Tiles: one per profile, plus a trailing "pair keyboard" tile. */
    tilegrid_t g = picker_grid(s.profile_count + 1);
    s.grid = g;
    if (s.sel >= g.count) s.sel = g.count ? g.count - 1 : 0;
    if (s.profile_count + 1 > g.ncols * g.nrows)
        ESP_LOGW(TAG, "%d profiles exceed one page; showing first %d",
                 s.profile_count, g.count - 1);

    for (int i = 0; i < g.count; i++) {
        int cx = tile_x(&g, i), cy = tile_y(&g, i);
        bool sel = (i == s.sel);
        if (i < s.profile_count) {
            const conn_profile_t *p = &s.profiles[i];
            char body[48];
            snprintf(body, sizeof(body), "%s@%s:%u%s",
                     p->user, p->host, (unsigned)p->port,
                     p->auth == STORAGE_AUTH_KEY ? "  [key]" : "");
            ui_pen(OVERLAY_COL_GREEN);
            ui_tile(cx, cy, g.tw, g.th, p->name, body, sel);
        } else {
            ui_pen(OVERLAY_COL_CYAN);
            ui_tile(cx, cy, g.tw, g.th, "+ Pair keyboard",
                    s.cfg.ble ? "tap or long-press" : "(no BLE)", sel);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    if (s.profile_count == 0)
        ui_puts(4, 5, "no profiles — edit profiles.ini in storage", 0);

    /* Footer strip. */
    draw_rule(ui_rows() - 2);
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(2, ui_rows() - 1, UI_PLAY, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, ui_rows() - 1,
            "tap to select, tap again to connect   hold = pair", 0);
    if (s.toast[0]) {
        int len = (int)strlen(s.toast) + 2;
        ui_pen(OVERLAY_COL_AMBER);
        ui_printf(ui_cols() - len - 1, ui_rows() - 1, OVERLAY_ATTR_INVERSE,
                  " %s ", s.toast);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    ui_no_cursor();
    ui_present();
}

/* Number of device tiles PAIRING shows (the rest of the page is the Cancel
 * tile, which is always the last slot). */
static int pairing_ndev(const tilegrid_t *g)
{
    return g->count - 1;
}

static void render_pairing(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "PAIR KEYBOARD", s.anim_frame);
    ui_pen(s.ndevs ? OVERLAY_COL_GREEN : OVERLAY_COL_AMBER);
    ui_putch(2, 1, s.ndevs ? UI_LED_ON : spinner_glyph(s.anim_frame), 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, 1, s.ndevs ? "select your keyboard below"
                          : "scanning for keyboards...", 0);
    draw_rule_scan(3, s.anim_frame);

    /* Devices + one Cancel tile; cap devices so Cancel always fits. */
    tilegrid_t g = picker_grid(1);            /* start with room for Cancel */
    int cap  = g.ncols * g.nrows;
    int ndev = s.ndevs > cap - 1 ? cap - 1 : s.ndevs;
    g.count  = ndev + 1;
    s.grid   = g;
    if (s.pair_sel >= g.count) s.pair_sel = g.count - 1;

    ui_pen(OVERLAY_COL_GREEN);
    for (int i = 0; i < ndev; i++) {
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                s.devs[i].name,
                s.devs[i].addr_type ? "random addr" : "public addr",
                i == s.pair_sel);
    }
    ui_pen(OVERLAY_COL_RED);
    ui_tile(tile_x(&g, ndev), tile_y(&g, ndev), g.tw, g.th,
            "Cancel", "", ndev == s.pair_sel);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_rule(ui_rows() - 2);
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(2, ui_rows() - 1, UI_PLAY, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, ui_rows() - 1, "put the keyboard in pairing mode, then tap it", 0);
    ui_no_cursor();
    ui_present();
}

/* Hostkey prompt buttons: two side-by-side tiles (trust/replace + cancel).
 * Slot 0 = trust/replace, slot 1 = cancel. */
static tilegrid_t hostkey_grid(void)
{
    tilegrid_t g = { .y0 = 18, .tw = 36, .th = 5,
                     .gx = 4, .gy = 0, .ncols = 2, .nrows = 1, .count = 2 };
    g.x0 = (ui_cols() - (g.tw * 2 + g.gx)) / 2;
    return g;
}

static void render_hostkey(void)
{
    ui_colors(s.fp_mismatch ? COLOR_WHITE : UI_FG,
              s.fp_mismatch ? RGB565(96, 0, 0) : UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    const conn_profile_t *p = &s.profiles[s.connect_idx];
    const char *fp = ssh_client_get_fingerprint();

    if (s.fp_mismatch) {
        ui_puts(4, 2, "!  HOST KEY CHANGED — possible attack  !", 0);
        ui_puts(4, 4, "The server's key DIFFERS from the pinned one.", 0);
        ui_puts(4, 5, "Only replace it if you KNOW the server was rekeyed.", 0);
    } else {
        ui_puts(4, 2, "Unknown host — first connection", 0);
        ui_printf(4, 4, 0, "First connection to %s:%u.", p->host, (unsigned)p->port);
        ui_puts(4, 5, "Verify the fingerprint before trusting it.", 0);
    }
    ui_puts(4, 7, "SHA256:", 0);
    char half[33];
    memcpy(half, fp, 32); half[32] = '\0';
    ui_puts(12, 7, half, 0);
    ui_puts(12, 8, fp + 32, 0);

    tilegrid_t g = hostkey_grid();
    s.grid = g;
    const char *trust = s.fp_mismatch
        ? (s.hostkey_armed ? "TAP AGAIN to REPLACE" : "Replace key")
        : "Trust & Connect";
    ui_pen(s.fp_mismatch ? OVERLAY_COL_AMBER : OVERLAY_COL_GREEN);
    ui_tile(tile_x(&g, 0), tile_y(&g, 0), g.tw, g.th, trust,
            s.fp_mismatch ? "danger" : "", s.hostkey_armed);
    ui_pen(s.fp_mismatch ? OVERLAY_COL_GREEN : OVERLAY_COL_DEFAULT);
    ui_tile(tile_x(&g, 1), tile_y(&g, 1), g.tw, g.th, "Cancel", "", false);
    ui_pen(OVERLAY_COL_DEFAULT);

    ui_puts(4, ui_rows() - 1, s.fp_mismatch
            ? "keyboard: Y = replace   Esc = cancel"
            : "keyboard: Enter = trust   Esc = cancel", 0);
    ui_no_cursor();
    ui_present();
}

static void render_connecting(const char *msg)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "CYBERDECK", s.anim_frame);
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 12, 0, "// SSH DECK", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    draw_rule(3);

    const conn_profile_t *p = &s.profiles[s.connect_idx];
    int cy = ui_rows() / 2;

    char line[96];
    snprintf(line, sizeof(line), "%s  %s@%s:%u", msg, p->user, p->host,
             (unsigned)p->port);
    int lx = (ui_cols() - (int)strlen(line)) / 2;
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(lx - 2, cy - 1, UI_DIAMOND, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(lx, cy - 1, line, 0);

    /* Cyberpunk "activity" bar: ░▒▓█▓▒░ repeating gradient. */
    static const uint16_t grad[7] = {
        UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK, UI_SHADE3, UI_SHADE2, UI_SHADE1
    };
    int bw = 42, bx = (ui_cols() - bw) / 2;
    ui_pen(OVERLAY_COL_CYAN);
    for (int i = 0; i < bw; i++)
        ui_putch(bx + i, cy + 1, grad[i % 7], 0);
    ui_pen(OVERLAY_COL_DEFAULT);

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
    ui_colors(UI_FG, UI_BG);
    ui_dim();   /* dim the live session behind the menu so it pops */

    tilegrid_t g = { .tw = 40, .th = 4, .gx = 0, .gy = 1,
                     .ncols = 1, .nrows = MENU_COUNT, .count = MENU_COUNT };
    g.x0 = (ui_cols() - g.tw) / 2;
    g.y0 = (ui_rows() - (MENU_COUNT * g.th + (MENU_COUNT - 1) * g.gy)) / 2;
    s.grid = g;

    static const uint8_t menu_col[MENU_COUNT] = {
        OVERLAY_COL_GREEN,   /* resume            */
        OVERLAY_COL_AMBER,   /* disconnect        */
        OVERLAY_COL_AMBER,   /* disconnect+profile */
        OVERLAY_COL_CYAN,    /* pair keyboard     */
    };
    for (int i = 0; i < MENU_COUNT; i++) {
        bool dim = (i == 3 && !s.cfg.ble);   /* no BLE on this platform */
        ui_pen(menu_col[i]);
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                menu_items[i], dim ? "(unavailable)" : "", i == s.menu_sel);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
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

/* Run the in-session menu action for the current selection. */
static void menu_activate(uint64_t now)
{
    switch (s.menu_sel) {
    case 0:                                   /* resume session */
        s.state = ST_SESSION;
        ui_hide();
        break;
    case 1:                                   /* disconnect */
        ssh_client_disconnect();
        enter_home(now);
        break;
    case 2:                                   /* disconnect + reload profiles */
        ssh_client_disconnect();
        load_profiles();
        enter_home(now);
        break;
    case 3:                                   /* pair keyboard (session lives on) */
        if (s.cfg.ble) enter_pairing(now);
        break;
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
    /* The terminal was cleared inside ssh_client_connect() before the read
     * task spawned — doing it here would race that task inside vterm. */
    toast(now, "connected — F12 for menu");
    render_session_toast(now);
}

static void do_connect(uint64_t now)
{
    static char key_path[160];
    static char pub_path[160];
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
        /* For a key profile the password field carries the key's passphrase
         * (empty = unencrypted key). */
        cfg.passphrase  = p->password[0] ? p->password : NULL;
        /* Pass a matching public key if the user dropped one next to the
         * private key — required for ECDSA (RSA derives its own). */
        snprintf(pub_path, sizeof(pub_path), "%s/keys/%s.pub",
                 storage_platform_mount_point(), p->key_id);
        FILE *pf = fopen(pub_path, "r");
        if (pf) { fclose(pf); cfg.public_key = pub_path; }
    } else {
        cfg.password = p->password;
    }

    esp_err_t err = ssh_client_connect(&cfg);

    switch (err) {
    case ESP_OK:
        enter_session(now);
        break;

    case SSH_ERR_HOSTKEY_UNKNOWN:
        s.fp_mismatch   = false;
        s.hostkey_armed = false;
        s.state = ST_HOSTKEY;
        render_hostkey();
        break;

    case SSH_ERR_HOSTKEY_MISMATCH:
        s.fp_mismatch   = true;
        s.hostkey_armed = false;
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

    s.anim_frame = (uint32_t)(now / ANIM_PERIOD_MS);

    switch (s.state) {
    case ST_BOOT:
        if (now >= s.boot_until) {
            enter_home(now);
            break;
        }
        if (now >= s.next_anim) {
            s.next_anim = now + ANIM_PERIOD_MS;
            render_boot(now);
        }
        break;

    case ST_HOME:
        if (now >= s.next_home_refresh) {
            s.next_home_refresh = now + ANIM_PERIOD_MS;   /* animation cadence */
            if (s.toast[0] && now >= s.toast_until) s.toast[0] = '\0';
            render_home();   /* live wifi/ble status + comet sweep */
        }
        break;

    case ST_PAIRING:
        if (now - s.pair_last_activity > PAIR_TIMEOUT_MS) {
            exit_pairing(now);
            break;
        }
        if (now >= s.next_anim) {          /* advance spinner / comet */
            s.next_anim = now + ANIM_PERIOD_MS;
            render_pairing();
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
        /* Long-press anywhere = open pairing (works without a keyboard). */
        if (ev->type == CYBERDECK_INPUT_LONG_PRESS) {
            if (s.cfg.ble) enter_pairing(now);
            break;
        }
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot < 0) break;                     /* gutter/margin: ignore */
            if (slot >= s.profile_count) {           /* the "pair keyboard" tile */
                if (s.cfg.ble) enter_pairing(now);
            } else if (s.sel != slot) {              /* first tap: select + show */
                s.sel = slot;
                render_home();
            } else if (!wifi_manager_is_connected()) {
                toast(now, "WiFi not connected yet");
                render_home();
            } else {                                 /* second tap on same tile */
                start_connect(slot, now, now);
            }
            break;
        }
        switch (k) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
            int ns = tile_nav(&s.grid, s.sel, k);
            if (ns != s.sel) { s.sel = ns; render_home(); }
            break;
        }
        case K_ENTER:
            if (s.sel >= s.profile_count) {          /* pair tile focused */
                if (s.cfg.ble) enter_pairing(now);
            } else if (s.profile_count > 0) {
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
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot < 0) break;                       /* gutter: ignore, keep scanning */
            if (slot < pairing_ndev(&s.grid) && s.cfg.ble) {
                s.cfg.ble->select_device(s.devs[slot].addr, s.devs[slot].addr_type);
                toast(now, "pairing %.32s...", s.devs[slot].name);
                exit_pairing(now);
            } else {                                   /* Cancel tile */
                exit_pairing(now);
            }
            break;
        }
        switch (k) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
            int ns = tile_nav(&s.grid, s.pair_sel, k);
            if (ns != s.pair_sel) { s.pair_sel = ns; render_pairing(); }
            break;
        }
        case K_ENTER:
            if (s.pair_sel < pairing_ndev(&s.grid) && s.cfg.ble) {
                s.cfg.ble->select_device(s.devs[s.pair_sel].addr,
                                         s.devs[s.pair_sel].addr_type);
                toast(now, "pairing %.32s...", s.devs[s.pair_sel].name);
                exit_pairing(now);   /* backend continues async */
            } else {
                exit_pairing(now);   /* Cancel tile focused */
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
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot == 1) {                         /* Cancel tile */
                enter_home(now);
            } else if (slot == 0) {                  /* Trust / Replace tile */
                if (!s.fp_mismatch || s.hostkey_armed)
                    hostkey_trust_and_connect(now);
                else { s.hostkey_armed = true; render_hostkey(); }  /* arm 2-tap */
            }
            break;
        }
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
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot < 0) {                    /* tap outside the menu = resume */
                s.state = ST_SESSION;
                ui_hide();
            } else {
                s.menu_sel = slot;
                menu_activate(now);            /* same as pressing Enter */
            }
            break;
        }
        switch (k) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
            int ns = tile_nav(&s.grid, s.menu_sel, k);
            if (ns != s.menu_sel) { s.menu_sel = ns; render_menu(); }
            break;
        }
        case K_ENTER:
            menu_activate(now);
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}
