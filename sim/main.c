/*
 * sim/main.c — PC simulator, vterm showcase demo.
 *
 * Three scenes cycle on SPACE:
 *   1. ANSI-16 foreground/background colors + SGR attributes
 *   2. Full 256-color palette (system-16, 6×6×6 cube, grayscale)
 *   3. System-monitor panel showcasing box-drawing + 256-color fills
 */

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "display.h"
#include "font.h"
#include "vterm.h"

#define SIM_COLS 100
#define SIM_ROWS  30

/* ---------- ANSI escape helpers ---------- */
#define ESC  "\x1b"
#define CSI  ESC "["
#define RST  CSI "0m"
#define BOLD CSI "1m"
#define UNDE CSI "4m"
#define INVR CSI "7m"
#define BLNK CSI "5m"
#define CLS  CSI "2J" CSI "H"

/* ---------- Box-drawing glyphs (UTF-8) ----------
 * Double-line outer frame                          */
#define OV  "\xe2\x95\x91"   /* ║ */
#define OH  "\xe2\x95\x90"   /* ═ */
#define OTL "\xe2\x95\x94"   /* ╔ */
#define OTR "\xe2\x95\x97"   /* ╗ */
#define OBL "\xe2\x95\x9a"   /* ╚ */
#define OBR "\xe2\x95\x9d"   /* ╝ */
#define OML "\xe2\x95\xa0"   /* ╠ */
#define OMR "\xe2\x95\xa3"   /* ╣ */
/* Single-line inner dividers                       */
#define IH  "\xe2\x94\x80"   /* ─ */
#define IV  "\xe2\x94\x82"   /* │ */
#define IsL "\xe2\x95\x9f"   /* ╟ double→single left  */
#define IsR "\xe2\x95\xa2"   /* ╢ double→single right */
#define CRS "\xe2\x94\xbc"   /* ┼ */
/* Block fill                                       */
#define BLK "\xe2\x96\x88"   /* █ full block  */
#define LIT "\xe2\x96\x91"   /* ░ light shade */

#define NUM_SCENES 3
static int s_scene;

/* ---------- Output helpers ---------- */
static void vw(const char *s) { vterm_write(s, strlen(s)); }

static void vf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vterm_write(buf, strlen(buf));
}

static void fg(int n)  { vf(CSI "38;5;%dm", n); }
static void bg(int n)  { vf(CSI "48;5;%dm", n); }
static void nl(void)   { vw("\r\n"); }
static void rep(const char *s, int n) { for (int i = 0; i < n; i++) vw(s); }

/* ---------- Shared scene header (full-width colour bar) ---------- */
static void scene_header(int n, const char *title)
{
    vw(CLS);
    bg(22); fg(255); vw(BOLD);
    vf("  %-58s  [%d/%d]  SPACE: next   ESC: quit  ",
       title, n + 1, NUM_SCENES);
    vw(CSI "K");    /* erase to EOL with current bg */
    vw(RST); nl();
}

/* ---------- Progress bar ---------- */
static void bar(int pct, int width, int color)
{
    int filled = pct * width / 100;
    fg(color);  rep(BLK, filled);
    fg(236);    rep(LIT, width - filled);
    vw(RST);
}

/* ==========================================================================
 * Scene 1 — ANSI-16 colors & SGR attributes
 * ======================================================================= */
static void scene_colors(void)
{
    static const char *names[16] = {
        "Black",   "Red",     "Green",   "Yellow",
        "Blue",    "Magenta", "Cyan",    "White",
        "Black",   "Red",     "Green",   "Yellow",
        "Blue",    "Magenta", "Cyan",    "White",
    };

    scene_header(0, "ANSI-16 colors & SGR attributes");
    nl();

    /* Foreground colors — 2 rows of 8 */
    fg(15); vw(BOLD "  Foreground colors:" RST); nl();
    for (int row = 0; row < 2; row++) {
        vw("  ");
        for (int i = row * 8; i < row * 8 + 8; i++) {
            fg(i); vf("%-12s", names[i]);
        }
        vw(RST); nl();
    }
    nl();

    /* Background colors — 2 rows of 8 */
    fg(15); vw(BOLD "  Background colors:" RST); nl();
    for (int row = 0; row < 2; row++) {
        vw("  ");
        for (int i = row * 8; i < row * 8 + 8; i++) {
            bg(i); fg((i >= 6 && i <= 10) ? 0 : 15);
            vf("  %2d:%-7s", i, names[i]);
            vw(RST);
        }
        nl();
    }
    vw(RST); nl();

    /* SGR attribute showcase */
    fg(15); vw(BOLD "  SGR attributes:" RST); nl();
    vw("  ");
    vw(               "Normal         ");
    vw(BOLD           "Bold"           RST "          ");
    vw(UNDE           "Underline"      RST "          ");
    vw(INVR           "Reverse"        RST "          ");
    vw(BLNK           "Blink"          RST "          ");
    vw(BOLD UNDE      "Bold+Underline" RST "   ");
    vw(BOLD INVR      "Bold+Reverse"   RST);
    nl(); nl();

    /* Styled sample text */
    fg(15); vw(BOLD "  Styled sample text:" RST); nl();
    static const int palette[] = { 9, 10, 11, 12, 13, 14 };
    for (int i = 0; i < 6; i++) {
        fg(palette[i]);
        vw("  The quick brown fox jumps over the lazy dog."
           "  0123456789  !@#$%^&*()_+-=");
        nl();
    }
    vw(RST);
}

/* ==========================================================================
 * Scene 2 — Full 256-color palette
 * ======================================================================= */
static void scene_256(void)
{
    scene_header(1, "256-color palette");
    nl();

    /* System colors 0-15 */
    fg(15); vw(BOLD "  System colors  0-15:" RST); nl();
    for (int row = 0; row < 2; row++) {
        vw("  ");
        for (int i = row * 8; i < row * 8 + 8; i++) {
            bg(i); fg((i >= 6 && i <= 10) ? 0 : 15);
            vf("  %3d  ", i);
            vw(RST " ");
        }
        nl();
    }
    nl();

    /* 6×6×6 color cube — 6 rows of 36 swatches (index = 16 + row*36 + col) */
    fg(15); vw(BOLD "  6×6×6 color cube  16-231:" RST); nl();
    for (int r = 0; r < 6; r++) {
        vw("  ");
        for (int c = 0; c < 36; c++) {
            bg(16 + r * 36 + c); vw("  "); vw(RST);
        }
        fg(239); vf("  %3d–%3d", 16 + r * 36, 16 + r * 36 + 35);
        vw(RST); nl();
    }
    nl();

    /* Grayscale ramp 232-255 — two rows of 12 */
    fg(15); vw(BOLD "  Grayscale ramp  232-255:" RST); nl();
    for (int row = 0; row < 2; row++) {
        vw("  ");
        for (int i = row * 12; i < row * 12 + 12; i++) {
            int idx = 232 + i;
            bg(idx); fg(idx < 244 ? 15 : 0);
            vf(" %3d ", idx);
            vw(RST);
        }
        nl();
    }
    vw(RST);
}

/* ==========================================================================
 * Scene 3 — System-monitor panel (box-drawing + 256-color fills)
 *
 * Outer frame: ╔ + 98×═ + ╗  = 100 cols exactly.
 * Content rows: ║ [98 chars] ║  using CSI G to place right border.
 * ======================================================================= */

/* Print the left ║ border; cursor lands at col 2. */
static void row_l(void)  { fg(238); vw(OV); vw(RST); }

/* Jump to col SIM_COLS, print right ║, newline. */
static void row_r(void)
{
    vf(CSI "%dG", SIM_COLS);
    fg(238); vw(OV); vw(RST); nl();
}

/* Full-width horizontal rules. */
static void hline_outer(const char *l, const char *r)
{
    fg(238); vw(l); rep(OH, 98); vw(r); vw(RST); nl();
}
static void hline_inner(void)
{
    fg(238); vw(IsL); rep(IH, 98); vw(IsR); vw(RST); nl();
}
/* Split inner rule: ╟ + at×─ + ┼ + (97-at)×─ + ╢ = 100 cols. */
static void hline_split(int at)
{
    fg(238); vw(IsL); rep(IH, at); vw(CRS); rep(IH, 97 - at); vw(IsR); vw(RST); nl();
}

static void scene_monitor(void)
{
    scene_header(2, "System monitor — box-drawing & 256-color demo");
    nl();

    /* ╔══════════╗ */
    hline_outer(OTL, OTR);

    /* Title row */
    row_l();
    bg(234); fg(46); vw(BOLD "  CYBERDECK SYSTEM MONITOR" RST);
    bg(234); fg(240); vw("                   ESP32-S3-Touch-LCD-7  @  240 MHz"); vw(RST);
    row_r();

    /* ╠══ Resources ══╣ */
    hline_outer(OML, OMR);
    row_l(); fg(250); vw(BOLD "  Resources" RST); row_r();
    hline_inner();

    static const struct {
        const char *label; int pct; int color; const char *info;
    } res[] = {
        { "CPU ", 58, 154, "  Xtensa LX7  240 MHz   tasks: 12 running " },
        { "HEAP", 76, 214, "  406 KB free / 512 KB total              " },
        { "PRAM", 35,  81, "  2.8 MB free / 4.0 MB total              " },
    };
    for (int i = 0; i < 3; i++) {
        row_l();
        fg(245); vw(BOLD "  "); vw(res[i].label); vw("  " RST);
        bar(res[i].pct, 32, res[i].color);
        fg(res[i].color); vf(BOLD "  %3d%%  " RST, res[i].pct);
        fg(250); vw(res[i].info);
        row_r();
    }

    /* ╠══ Network | Terminal ══╣
     * Split at column 41: left=40 chars, │ at 42, right=57 chars.        */
    hline_outer(OML, OMR);
    row_l();
    fg(250); vw(BOLD "  Network" RST);
    vf(CSI "42G"); fg(238); vw(IV); vw(RST);
    fg(250); vw(BOLD "  Terminal" RST);
    row_r();
    hline_split(40);  /* ╟────────────────────────────────────────┼─────...─╢ */

    static const char *net[4] = {
        "  SSID:  CyberNet_5G ",
        "  IP:    192.168.1.42",
        "  RSSI:  54 dBm      ",
        "  Auth:  publickey   ",
    };
    static const char *term[4] = {
        "  Backend:    libtsm 4.3.0",
        "  Emulation:  VT220 / xterm-256color",
        "  Size:       100×30   UTF-8",
        "  SSH:        cyberdeck@dev.example.com:22",
    };
    for (int i = 0; i < 4; i++) {
        row_l();
        fg(252); vf("%-40s", net[i]); vw(RST);
        fg(238); vw(IV); vw(RST);
        fg(252); vw(term[i]);
        row_r();
    }

    /* ╠══ Color palette ══╣ */
    hline_outer(OML, OMR);
    row_l(); fg(250); vw(BOLD "  Color palette" RST); row_r();
    hline_inner();

    /* 256-color gradient — map 0-255 linearly to 96 single-char swatches. */
    row_l();
    fg(245); vw("  ");
    for (int i = 0; i < 96; i++) {
        bg(i * 255 / 95); vw(" "); vw(RST);
    }
    row_r();

    /* System-16 swatches + grayscale ramp side by side. */
    row_l();
    fg(245); vw("  Sys-16  ");
    for (int i = 0; i < 16; i++) { bg(i); vw("  "); vw(RST); }
    fg(245); vw("   Gray-24  ");
    for (int i = 232; i < 256; i++) { bg(i); vw(" "); vw(RST); }
    row_r();

    /* ╚══════════╝ */
    hline_outer(OBL, OBR);
}

/* ==========================================================================
 * Main
 * ======================================================================= */
static void show_scene(int n)
{
    switch (n) {
        case 0: scene_colors();  break;
        case 1: scene_256();     break;
        case 2: scene_monitor(); break;
    }
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    font_init();

    if (display_init() != ESP_OK) {
        fprintf(stderr, "display_init failed\n");
        SDL_Quit();
        return 1;
    }

    if (vterm_init(SIM_COLS, SIM_ROWS) != ESP_OK) {
        fprintf(stderr, "vterm_init failed\n");
        SDL_Quit();
        return 1;
    }

    show_scene(s_scene);

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        running = false;
                        break;
                    case SDLK_SPACE:
                        s_scene = (s_scene + 1) % NUM_SCENES;
                        show_scene(s_scene);
                        break;
                    default:
                        break;
                }
            }
        }
        display_render_frame();
        SDL_Delay(16);
    }

    SDL_Quit();
    return 0;
}
