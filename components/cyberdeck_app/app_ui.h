/*
 * app_ui.h — overlay TUI primitives for the shell (internal to cyberdeck_app).
 *
 * All shell UI is drawn into the display overlay layer, composited by the
 * render core above the vterm cell buffer. The vterm buffer belongs to the
 * SSH session (and boot splash) alone — shell chrome never corrupts it.
 */

#pragma once

#include "display.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Unicode box-drawing codepoints (present in terminus8x16) */
#define UI_BOX_H   0x2500u  /* ─ */
#define UI_BOX_V   0x2502u  /* │ */
#define UI_BOX_TL  0x250Cu  /* ┌ */
#define UI_BOX_TR  0x2510u  /* ┐ */
#define UI_BOX_BL  0x2514u  /* └ */
#define UI_BOX_BR  0x2518u  /* ┘ */
#define UI_BOX_ML  0x251Cu  /* ├ */
#define UI_BOX_MR  0x2524u  /* ┤ */

/** Allocate the overlay buffer for the current display size. */
esp_err_t ui_init(void);

int  ui_cols(void);
int  ui_rows(void);

/** Publish the drawn frame (double-buffered, atomic) and swap. */
void ui_present(void);
/** Unregister the overlay so the terminal buffer shows through. */
void ui_hide(void);
bool ui_visible(void);

/** Park the terminal cursor off-screen (call in full-screen modals). */
void ui_no_cursor(void);

/** Set the two overlay colors (all cells share them; INVERSE swaps). */
void ui_colors(color_t fg, color_t bg);

/** Clear the whole overlay to transparent. */
void ui_clear(void);

/** Put one codepoint; attrs = 0 or OVERLAY_ATTR_INVERSE. */
void ui_putch(int col, int row, uint16_t cp, uint8_t attrs);

/** Put an ASCII/Latin-1 string. */
void ui_puts(int col, int row, const char *s, uint8_t attrs);

/** printf into a row (ASCII), truncated to the overlay width. */
void ui_printf(int col, int row, uint8_t attrs, const char *fmt, ...);

/** Horizontal rule: left_cp fill_cp... right_cp */
void ui_hline(int col, int row, int width,
              uint16_t left_cp, uint16_t fill_cp, uint16_t right_cp);

/** Fill a rectangle with spaces (opaque background). */
void ui_fill(int col, int row, int w, int h, uint8_t attrs);

/** Box with border and title centered in the top rule. */
void ui_box(int col, int row, int w, int h, const char *title);
