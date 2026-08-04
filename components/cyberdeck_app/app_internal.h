/*
 * app_internal.h — the shell's shared spine (internal to cyberdeck_app).
 *
 * The one shared instance (`app`, defined in cyberdeck_app.c) holds only
 * state that genuinely crosses modules: config, FSM state, the profile
 * list, the current tile grid, the toast and the animation clock. Every
 * screen keeps its own state as statics in its app_*.c module; the
 * cross-module entry points are declared in app_screens.h.
 */

#pragma once

#include "cyberdeck_app.h"
#include "app_ui.h"
#include "storage.h"

typedef enum {
    ST_BOOT = 0,
    ST_HOME,        /* profile picker + status                       */
    ST_PAIRING,     /* BLE keyboard scan list (modal)                */
    ST_HOSTKEY,     /* trust-on-first-use fingerprint prompt (modal) */
    ST_CONNECTING,  /* pending/armed SSH connect                     */
    ST_SESSION,     /* bytes flow to/from SSH                        */
    ST_POWEROFF,    /* CRT collapse playing over the dead session    */
    ST_MENU,        /* in-session overlay menu                       */
    ST_WIFIPROV,    /* SoftAP WiFi onboarding (modal)                */
    ST_PROFILE,     /* on-device profile editor (modal)              */
    ST_SSHIMPORT,   /* SoftAP + HTTP SSH-profile import (modal)      */
    ST_COUNT,
} app_state_t;

#define MAX_PROFILES     (STORAGE_MAX_PROFILES + 1)   /* stored + synth fallback */

#define ANIM_PERIOD_MS   100          /* ~10 fps subtle UI animation */
#define TOAST_MS         3000         /* status trivia */
#define ERR_TOAST_MS     7000         /* errors the user must actually read */
#define MENU_MSG_MS      5000         /* menu action feedback lifetime */

/* Shell palette — VGA phosphor green on black. Per-cell accents
 * (OVERLAY_COL_*) layer the rest of the classic 16 on top. */
#define UI_FG   RGB565(85, 255, 85)
#define UI_BG   RGB565(0, 0, 0)

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* A page of finger-sized tiles laid out in a grid, with two-axis touch
 * hit-testing. Recomputed by each render_*() and saved for the tap handler.
 * All dimensions are in character cells. */
typedef struct {
    int x0, y0;        /* top-left cell of the grid            */
    int tw, th;        /* tile size in cells                   */
    int gx, gy;        /* gutter between tiles, in cells       */
    int ncols, nrows;  /* tiles per page                       */
    int count;         /* live tiles on this page (<= ncols*nrows) */
} tilegrid_t;

/* Decoded UI keys (core decodes once per event; screens get the result). */
typedef enum {
    K_NONE = 0, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_ENTER, K_ESC, K_F12, K_CHAR, K_BACKSPACE, K_TAB,
} ui_key_t;

struct app_state {
    cyberdeck_app_config_t cfg;
    app_state_t state;

    conn_profile_t profiles[MAX_PROFILES];
    int  profile_count;
    int  stored_count;              /* profiles actually on flash (excl. synth) */

    /* Tile grid of the current screen, saved for touch hit-testing. */
    tilegrid_t grid;

    /* toast (SESSION only; UI states draw status inline) */
    char     toast[64];
    uint64_t toast_until;
    bool     toast_ok;     /* success toast: spinner-to-checkmark garnish */

    uint32_t anim_frame;   /* advances ~10 fps for subtle animation */
    uint64_t next_anim;    /* next animated re-render */
};

extern struct app_state app;

/* ------------------------------------------------- core services (cyberdeck_app.c) */

/** (Re)load stored profiles into app.profiles (+ Kconfig fallback synth). */
void load_profiles(void);

/** True if a keyboard is bonded (present in the BLE registry). */
bool ble_has_bond(void);

/** Connect wifi_manager from wifi.ini (or the Kconfig fallback). */
void kick_wifi(void);

/** Post a toast for @p ms; drawn by HOME inline or the SESSION toast chip. */
void toast_for(uint64_t now, uint32_t ms, const char *fmt, ...);

/* Status trivia keeps the short default; errors the user must actually read
 * (auth failures, drop reasons) call toast_for() with ERR_TOAST_MS. */
#define toast(now, ...)  toast_for(now, TOAST_MS, __VA_ARGS__)
