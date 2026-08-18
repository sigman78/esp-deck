/*
 * app_unlock.c — keystore code entry (ST_UNLOCK).
 *
 * One screen, four phases: UNLOCK (the PIN pad's day job), and the set-code
 * flow OLD → NEW → CONFIRM reached from the menu (create skips OLD). 3x4
 * pad (touch) with BLE-keyboard parity: digits append, Backspace deletes,
 * Enter submits; a passphrase slot is reachable by just typing it.
 * Auto-submits at the known PIN length (keystore_pin_len header hint).
 *
 * Every keystore derivation runs on a worker task — Argon2id takes ~1 s on
 * the S3 and keeps ~1 KiB + hash state on the caller's stack
 * (docs/storage_auth.md), so it must never run inline in a UI tick. The
 * screen animates "DERIVING KEY" meanwhile and swallows input (the
 * derivation is not cancellable). change-code = two derivations (~2 s).
 *
 * Entered from: start_connect() lazily (locked key profile), the saver wake
 * gate and the boot trigger (lock.ini toggles), and the menu's KEYSTORE
 * page (set/change code). Failed-attempt backoff is a later slice.
 */

#include "app_internal.h"
#include "app_menu_defs.h"   /* MS_KEYSTORE (the menu return target) */
#include "app_screens.h"
#include "app_widgets.h"
#include "display_fx.h"
#include "keystore.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NOTE_MS      1200         /* status-row flash duration        */
#define CODE_MIN     4            /* UI floor for a new code's length */

/* Entry phase (app.unlock.mode). */
enum { UM_UNLOCK = 0, UM_OLD, UM_NEW, UM_CONFIRM };

/* Where to land when the screen closes (app.unlock.ret). */
enum { UR_HOME = 0, UR_CONNECT, UR_MENU };

/* Worker op — the keystore call made on the worker task. */
enum { OP_UNLOCK = 0, OP_CREATE, OP_CHANGE };

/* Worker exchange + flow stashes — .bss (internal SRAM), wiped as soon as
 * each value has served its purpose. */
static char          s_try[sizeof(((unlock_state_t *)0)->code)];
static char          s_old[sizeof(s_try)];      /* change flow: current code */
static char          s_staged[sizeof(s_try)];   /* set flow: new, pre-confirm */
static uint8_t       s_op;
static volatile bool s_done;
static volatile int  s_result;

static void unlock_worker(void *arg)
{
    (void)arg;
    esp_err_t r;
    switch (s_op) {
    case OP_CREATE: r = keystore_create(s_try);            break;
    case OP_CHANGE: r = keystore_change_pin(s_old, s_try); break;
    default:        r = keystore_unlock(s_try);            break;
    }
    memset(s_try, 0, sizeof(s_try));
    memset(s_old, 0, sizeof(s_old));
    s_result = r;
    s_done   = true;
    vTaskDelete(NULL);
}

/* Pad slots: 1 2 3 / 4 5 6 / 7 8 9 / DEL 0 OK. */
static const char *PAD_LBL[12] = { "1", "2", "3", "4", "5", "6",
                                   "7", "8", "9", "DEL", "0", "OK" };

static const char *phase_title(void)
{
    switch (app.unlock.mode) {
    case UM_OLD:     return "CURRENT ACCESS CODE";
    case UM_NEW:     return "NEW ACCESS CODE";
    case UM_CONFIRM: return "CONFIRM NEW CODE";
    default:         return "ENTER ACCESS CODE";
    }
}

static void render_unlock(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    bool tall = ui_rows() >= 24;
    draw_titlebar(2, phase_title());
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 12, 0, "// KEYSTORE", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (tall) draw_rule(3);

    tilegrid_t g = { .gx = 2, .gy = 0, .ncols = 3, .nrows = 4, .count = 12 };
    g.th = ui_rows() >= 28 ? 4 : 3;
    g.tw = ui_cols() >= 80 ? 10 : 8;
    int dots_row = tall ? 5 : 2;
    g.y0 = dots_row + 2;
    g.x0 = (ui_cols() - (g.tw * 3 + g.gx * 2)) / 2;
    app.grid = g;

    /* Status / dots row: the code being typed, or what the deck is doing
     * with it. A note (wrong code, mismatch) flashes until input resumes. */
    if (app.unlock.deriving) {
        static const char MSG[] = "DERIVING KEY";
        int x = (ui_cols() - (int)sizeof(MSG) + 1) / 2;
        ui_pen(OVERLAY_COL_CYAN);
        ui_putch(x - 2, dots_row, spinner_glyph(app.anim_frame), 0);
        ui_puts(x, dots_row, MSG, 0);
    } else if (app.unlock.note_until) {
        uint8_t blink = ((app.anim_frame / 3) & 1) ? OVERLAY_ATTR_INVERSE : 0;
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts((ui_cols() - (int)strlen(app.unlock.note)) / 2, dots_row,
                app.unlock.note, blink);
    } else {
        /* Fixed slots when the length is known (filled ● / hollow ○);
         * free-length entry grows dot by dot behind a blinking caret. */
        int n = app.unlock.expected ? app.unlock.expected : app.unlock.len;
        if (n > 24) n = 24;
        int x0 = (ui_cols() - (n * 2 + (app.unlock.expected ? -1 : 1))) / 2;
        for (int i = 0; i < n; i++) {
            bool filled = i < app.unlock.len;
            ui_pen(filled ? OVERLAY_COL_GREEN : OVERLAY_COL_DEFAULT);
            ui_putch(x0 + i * 2, dots_row, filled ? UI_LED_ON : UI_LED_OFF, 0);
        }
        if (!app.unlock.expected && ((app.anim_frame / 4) & 1)) {
            ui_pen(OVERLAY_COL_GREEN);
            ui_putch(x0 + n * 2, dots_row, UI_VBAR, 0);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    for (int i = 0; i < 12; i++) {
        ui_pen(i == 9 ? OVERLAY_COL_AMBER :
               i == 11 ? OVERLAY_COL_GREEN : OVERLAY_COL_CYAN);
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                PAD_LBL[i], "", false);
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer(app.unlock.deriving ? "working..."
              : app.unlock.mode == UM_NEW
                  ? "4+ chars \xB7 Enter OK \xB7 Esc cancel"
                  : "digits \xB7 Enter OK \xB7 Esc cancel");
    ui_no_cursor();
    ui_present();
}

static void flash_note(uint64_t now, const char *msg)
{
    app.unlock.note       = msg;
    app.unlock.note_until = now + NOTE_MS;
    app.next_anim = 0;
}

static void wipe_entry(void)
{
    memset(app.unlock.code, 0, sizeof(app.unlock.code));
    app.unlock.len = 0;
}

/* Land wherever this screen was opened from. */
static void unlock_finish(uint64_t now)
{
    wipe_entry();
    memset(s_staged, 0, sizeof(s_staged));
    switch (app.unlock.ret) {
    case UR_CONNECT: connect_resume_active(now); break;
    case UR_MENU:    app.state = ST_MENU; menu_goto(MS_KEYSTORE); break;
    default:         enter_home(now); break;
    }
}

static void unlock_cancel(uint64_t now)
{
    memset(s_old, 0, sizeof(s_old));
    if (app.unlock.ret == UR_CONNECT) {
        toast(now, "cancelled");
        app.unlock.ret = UR_HOME;      /* don't arm the connect */
    }
    unlock_finish(now);
}

static void start_worker(uint64_t now, uint8_t op)
{
    memcpy(s_try, app.unlock.code, sizeof(s_try));
    wipe_entry();
    s_op   = op;
    s_done = false;
    app.unlock.deriving = true;
    if (xTaskCreatePinnedToCore(unlock_worker, "ks_unlock", 8192, NULL,
                                5, NULL, 0) != pdPASS) {
        memset(s_try, 0, sizeof(s_try));
        memset(s_old, 0, sizeof(s_old));
        app.unlock.deriving = false;
        toast_for(now, ERR_TOAST_MS, "unlock worker failed");
        enter_home(now);
        return;
    }
    app.next_anim = 0;
    render_unlock();
}

/* Auto-submit length for the CONFIRM phase: mirror an all-digit new code so
 * the confirm feels like the pad it is; passphrases submit on Enter. */
static uint8_t staged_expected(void)
{
    for (const char *p = s_staged; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    size_t n = strlen(s_staged);
    return n <= 24 ? (uint8_t)n : 0;
}

static void enter_phase(uint8_t mode, uint8_t expected)
{
    wipe_entry();
    app.unlock.mode     = mode;
    app.unlock.expected = expected;
    app.next_anim = 0;
    render_unlock();
}

static void submit(uint64_t now)
{
    if (app.unlock.len == 0 || app.unlock.deriving) return;
    switch (app.unlock.mode) {
    case UM_OLD:                       /* stash, then ask for the new code */
        memcpy(s_old, app.unlock.code, sizeof(s_old));
        enter_phase(UM_NEW, 0);
        return;
    case UM_NEW:
        if (app.unlock.len < CODE_MIN) {
            flash_note(now, "AT LEAST 4 CHARACTERS");
            return;                    /* entry kept — extend it */
        }
        memcpy(s_staged, app.unlock.code, sizeof(s_staged));
        enter_phase(UM_CONFIRM, staged_expected());
        return;
    case UM_CONFIRM:
        if (strcmp(app.unlock.code, s_staged) != 0) {
            memset(s_staged, 0, sizeof(s_staged));
            enter_phase(UM_NEW, 0);
            flash_note(now, "CODES DON'T MATCH");
            return;
        }
        memcpy(app.unlock.code, s_staged, sizeof(app.unlock.code));
        memset(s_staged, 0, sizeof(s_staged));
        start_worker(now, app.unlock.creating ? OP_CREATE : OP_CHANGE);
        return;
    default:
        start_worker(now, OP_UNLOCK);
        return;
    }
}

static void append_char(char c, uint64_t now)
{
    app.unlock.note_until = 0;
    if (app.unlock.len >= (int)sizeof(app.unlock.code) - 1) return;
    app.unlock.code[app.unlock.len++] = c;
    if (app.unlock.expected && app.unlock.len == app.unlock.expected) {
        submit(now);
        return;
    }
    render_unlock();
}

static void erase_char(void)
{
    app.unlock.note_until = 0;
    if (app.unlock.len > 0)
        app.unlock.code[--app.unlock.len] = '\0';
    render_unlock();
}

void unlock_open(uint64_t now, bool resume_connect)
{
    (void)now;
    app.unlock.ret      = resume_connect ? UR_CONNECT : UR_HOME;
    app.unlock.creating = false;
    app.unlock.deriving = false;
    app.unlock.note_until = 0;
    app.state = ST_UNLOCK;
    enter_phase(UM_UNLOCK, keystore_pin_len());
}

void unlock_open_setpin(uint64_t now)
{
    (void)now;
    app.unlock.ret      = UR_MENU;
    app.unlock.creating = keystore_state() == KEYSTORE_ABSENT;
    app.unlock.deriving = false;
    app.unlock.note_until = 0;
    app.state = ST_UNLOCK;
    if (app.unlock.creating) enter_phase(UM_NEW, 0);
    else                     enter_phase(UM_OLD, keystore_pin_len());
}

/* The worker finished — route the result by op. */
static void worker_result(uint64_t now, esp_err_t r)
{
    if (s_op == OP_UNLOCK) {
        if (r == ESP_OK) {
            toast(now, "keystore unlocked");
            unlock_finish(now);
        } else if (r == ESP_FAIL) {                 /* wrong code */
            display_bell();
            display_fx_static_brief();
            flash_note(now, "ACCESS DENIED");
        } else {                                    /* corrupt / OOM / gone */
            toast_for(now, ERR_TOAST_MS, "keystore error (%d)", (int)r);
            enter_home(now);
        }
        return;
    }
    if (r == ESP_OK) {
        toast(now, s_op == OP_CREATE ? "keystore created" : "code changed");
        unlock_finish(now);
        return;
    }
    if (s_op == OP_CHANGE && r == ESP_FAIL) {       /* wrong current code */
        display_bell();
        display_fx_static_brief();
        enter_phase(UM_OLD, keystore_pin_len());
        flash_note(now, "WRONG CURRENT CODE");
        return;
    }
    toast_for(now, ERR_TOAST_MS, "keystore error (%d)", (int)r);
    unlock_finish(now);
}

void unlock_tick(uint64_t now)
{
    if (app.unlock.deriving && s_done) {
        app.unlock.deriving = false;
        wipe_entry();
        worker_result(now, (esp_err_t)s_result);
        if (app.state != ST_UNLOCK) return;
    }
    if (app.unlock.note_until && now >= app.unlock.note_until)
        app.unlock.note_until = 0;

    if (now >= app.next_anim) {                     /* spinner/caret/blink */
        app.next_anim = now + ANIM_PERIOD_MS;
        render_unlock();
    }
}

void unlock_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    if (app.unlock.deriving) return;       /* KDF running: not cancellable */

    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) return;
        if (slot == 9)       erase_char();
        else if (slot == 11) submit(now);
        else                 append_char(slot == 10 ? '0' : (char)('1' + slot),
                                         now);
        return;
    }
    if (ev->type != CYBERDECK_INPUT_KEY) return;

    if (k == K_ESC)            unlock_cancel(now);
    else if (k == K_ENTER)     submit(now);
    else if (k == K_BACKSPACE) erase_char();
    else if (k == K_CHAR)      append_char(ch, now);
}
