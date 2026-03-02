/*
 * tsm_slim — lightweight VT terminal for ESP32-S3
 *
 * Public header.  Grows with each phase:
 *   Phase 1 — cell type, forward declarations
 *   Phase 2 — termstate API (tsm_t, tsm_new, tsm_feed …)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Cell ────────────────────────────────────────────────────────────────────
 *
 * 8 bytes, 4-byte aligned.  Binary-compatible with the existing
 * terminal_cell_t used by display_render_chunk() so a cast is safe
 * (uint16_t cp lives at offset 0 in both structs).
 *
 *  word[15: 0]  Unicode codepoint (BMP, U+0000–U+FFFF)
 *  word[20:16]  reserved (SMP extension, unused)
 *  word[   21]  CELL_FLAG_WIDE_RIGHT  — right-half placeholder of wide glyph
 *  word[   22]  CELL_FLAG_PROTECTED   — DECSCA protected cell
 *  word[   23]  CELL_ATTR_BOLD
 *  word[   24]  CELL_ATTR_DIM
 *  word[   25]  CELL_ATTR_ITALIC
 *  word[   26]  CELL_ATTR_UNDERLINE
 *  word[   27]  CELL_ATTR_BLINK
 *  word[   28]  CELL_ATTR_INVERSE
 *  word[   29]  CELL_ATTR_INVISIBLE
 *  word[   30]  CELL_ATTR_STRIKE
 *  word[   31]  CELL_ATTR_OVERLINE
 */
typedef struct {
    uint32_t word;   /* codepoint + flags + attrs (see above) */
    uint16_t fg;     /* foreground RGB565, pre-converted at SGR parse time */
    uint16_t bg;     /* background RGB565, pre-converted at SGR parse time */
} tsm_cell_t;

/* Codepoint access */
#define CELL_CP(c)        ((uint16_t)((c).word & 0xFFFFu))
#define CELL_SET_CP(c,v)  ((c).word = ((c).word & ~0xFFFFu) | ((uint16_t)(v)))

/* Flags */
#define CELL_FLAG_WIDE_RIGHT  (1u << 21)
#define CELL_FLAG_PROTECTED   (1u << 22)

/* Attributes (SGR) */
#define CELL_ATTR_BOLD        (1u << 23)
#define CELL_ATTR_DIM         (1u << 24)
#define CELL_ATTR_ITALIC      (1u << 25)
#define CELL_ATTR_UNDERLINE   (1u << 26)
#define CELL_ATTR_BLINK       (1u << 27)
#define CELL_ATTR_INVERSE     (1u << 28)
#define CELL_ATTR_INVISIBLE   (1u << 29)
#define CELL_ATTR_STRIKE      (1u << 30)
#define CELL_ATTR_OVERLINE    (1u << 31)

#define CELL_ATTRS_MASK       (0xFF800000u)

/* ── Dirty row segment ───────────────────────────────────────────────────────
 *
 * Tracks the leftmost and rightmost columns written since the last
 * tsm_clear_dirty() call.  l > r means the row is clean.
 */
typedef struct {
    uint8_t l;   /* leftmost dirty column (inclusive) */
    uint8_t r;   /* rightmost dirty column (inclusive); l > r → clean */
} tsm_row_dirty_t;

/* Clean sentinel values */
#define TSM_DIRTY_L_CLEAN  0xFFu
#define TSM_DIRTY_R_CLEAN  0x00u
