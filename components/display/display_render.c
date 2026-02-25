/*
 * display_render.c — shared rendering core.
 *
 * Compiled for BOTH the ESP32 target (called from the bounce-buffer ISR)
 * and the PC simulator (called from the SDL2 frame loop).
 *
 * No SDL headers, no esp_lcd headers — only display.h and font.h.
 */

#include "display_render.h"
#include "font.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * Cell buffer — registered once by terminal_init via display_set_text_buffer.
 * Plain statics (DRAM on ESP32, regular BSS on host) so the ISR can reach them.
 * ---------------------------------------------------------------------- */
static DRAM_ATTR const terminal_cell_t *s_cell_buf  = NULL;
static DRAM_ATTR int                    s_cell_cols = 0;
static DRAM_ATTR int                    s_cell_rows = 0;

/* -------------------------------------------------------------------------
 * Per-column rendering cache.
 * Static (not stack) to avoid blowing the ISR stack on ESP32.
 * DRAM_ATTR keeps it in internal SRAM — reachable from ISR without Flash cache.
 * ---------------------------------------------------------------------- */
#define RENDER_MAX_COLS  (DISPLAY_WIDTH / FONT_WIDTH)   /* 100 */

static DRAM_ATTR struct {
    const uint8_t *glyph;   /* 16-byte bitmap in DRAM (terminus8x16)     */
    uint16_t       bg;      /* background colour RGB565                   */
    uint16_t       xorfg;   /* bg ^ fg — XOR in to flip bg→fg per bit    */
} s_col_cache[RENDER_MAX_COLS];

/* -------------------------------------------------------------------------
 * Branchless pixel-pair → 32-bit word (little-endian, 2×RGB565).
 *
 *   bit   = (gb >> (7-p)) & 1
 *   mask  = 0xFFFF if bit=1, 0x0000 if bit=0  →  (uint16_t)(0u - bit)
 *   pixel = bg ^ (xorfg & mask)        → bg when bit=0, fg when bit=1
 *
 * Left pixel in low 16 bits, right pixel in high 16 bits (little-endian).
 * ---------------------------------------------------------------------- */
#define GPAIR(gb, p0, p1, bg_v, xor_v)                                          \
    (   (uint32_t)((uint16_t)((bg_v) ^ ((xor_v) &                               \
            (uint16_t)(0u - (((unsigned)(gb) >> (7u - (p0))) & 1u)))))           \
    |  ((uint32_t)((uint16_t)((bg_v) ^ ((xor_v) &                               \
            (uint16_t)(0u - (((unsigned)(gb) >> (7u - (p1))) & 1u))))) << 16) )

/* -------------------------------------------------------------------------
 * ANSI-256 colour → RGB565.
 * IRAM_ATTR: callable from ESP32 ISR without going through Flash cache.
 * ---------------------------------------------------------------------- */
color_t IRAM_ATTR ansi_to_rgb565(uint8_t ansi_color)
{
    /* 0-15: standard 16 colours */
    static DRAM_ATTR const color_t ansi_palette[16] = {
        RGB565(0,   0,   0  ),  /*  0 Black             */
        RGB565(128, 0,   0  ),  /*  1 Red               */
        RGB565(0,   128, 0  ),  /*  2 Green             */
        RGB565(128, 128, 0  ),  /*  3 Yellow            */
        RGB565(0,   0,   128),  /*  4 Blue              */
        RGB565(128, 0,   128),  /*  5 Magenta           */
        RGB565(0,   128, 128),  /*  6 Cyan              */
        RGB565(192, 192, 192),  /*  7 White             */
        RGB565(128, 128, 128),  /*  8 Bright Black/Gray */
        RGB565(255, 0,   0  ),  /*  9 Bright Red        */
        RGB565(0,   255, 0  ),  /* 10 Bright Green      */
        RGB565(255, 255, 0  ),  /* 11 Bright Yellow     */
        RGB565(0,   0,   255),  /* 12 Bright Blue       */
        RGB565(255, 0,   255),  /* 13 Bright Magenta    */
        RGB565(0,   255, 255),  /* 14 Bright Cyan       */
        RGB565(255, 255, 255),  /* 15 Bright White      */
    };

    if (ansi_color < 16) {
        return ansi_palette[ansi_color];
    }

    /* 16-231: 6×6×6 RGB cube */
    if (ansi_color <= 231) {
        uint8_t idx = ansi_color - 16;
        uint8_t r = (idx / 36) * 51;
        uint8_t g = ((idx / 6) % 6) * 51;
        uint8_t b = (idx % 6) * 51;
        return RGB565(r, g, b);
    }

    /* 232-255: grayscale ramp */
    uint8_t gray = 8 + (ansi_color - 232) * 10;
    return RGB565(gray, gray, gray);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows)
{
    s_cell_buf  = buf;
    s_cell_cols = cols;
    s_cell_rows = rows;
}

/**
 * Render one horizontal band (one character-row height) into dst.
 *
 * pos_px   — index of first pixel in the full framebuffer
 *            (= start_scanline × DISPLAY_WIDTH)
 * n_bytes  — byte count of the band
 *            (= DISPLAY_WIDTH × FONT_HEIGHT × sizeof(color_t) per chunk)
 */
void IRAM_ATTR display_render_chunk(color_t *dst, int pos_px, int n_bytes)
{
    if (!dst) return;

    /* Cell buffer not yet registered — fill black. */
    if (!s_cell_buf || s_cell_cols <= 0 || s_cell_rows <= 0) {
        uint32_t *p = (uint32_t *)dst;
        int words = n_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        return;
    }

    const int start_scan = pos_px / DISPLAY_WIDTH;
    const int num_scans  = (n_bytes >> 1) / DISPLAY_WIDTH;  /* n_bytes/2 = pixels */
    const int char_row   = start_scan / FONT_HEIGHT;

    /* Below the text area — fill black. */
    if (char_row >= s_cell_rows) {
        uint32_t *p = (uint32_t *)dst;
        int words = n_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        return;
    }

    /* ------------------------------------------------------------------
     * Build per-column cache for this character row.
     * ansi_to_rgb565 and font_get_glyph are both IRAM_ATTR; their data
     * is DRAM_ATTR — no Flash access.
     * ------------------------------------------------------------------ */
    const terminal_cell_t *row_cells = s_cell_buf + char_row * s_cell_cols;
    const int ncols = s_cell_cols;

    for (int c = 0; c < ncols; c++) {
        const terminal_cell_t *cell = &row_cells[c];
        color_t fg = ansi_to_rgb565(cell->fg_color);
        color_t bg = ansi_to_rgb565(cell->bg_color);
        if (cell->attrs & ATTR_REVERSE) { color_t t = fg; fg = bg; bg = t; }
        s_col_cache[c].glyph = font_get_glyph(cell->cp);
        s_col_cache[c].bg    = bg;
        s_col_cache[c].xorfg = (uint16_t)(fg ^ bg);
    }

    /* ------------------------------------------------------------------
     * Render scanlines.
     * glyph scanline index == scanline index within the band (n)
     * because the band always starts on a char-row boundary.
     *
     * Inner loop: two adjacent columns per iteration → 16 pixels
     * → 8 × uint32_t writes, naturally aligned.
     * ------------------------------------------------------------------ */
    color_t *dst_base = dst;

    for (int n = 0; n < num_scans; n++) {
        const uint8_t gl = (uint8_t)n;
        uint32_t *d = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH);

        int c = 0;
        for (; c + 1 < ncols; c += 2) {
            const uint8_t b0 = s_col_cache[c    ].glyph ? s_col_cache[c    ].glyph[gl] : 0u;
            const uint8_t b1 = s_col_cache[c + 1].glyph ? s_col_cache[c + 1].glyph[gl] : 0u;
            const uint16_t bg0 = s_col_cache[c    ].bg,  xf0 = s_col_cache[c    ].xorfg;
            const uint16_t bg1 = s_col_cache[c + 1].bg,  xf1 = s_col_cache[c + 1].xorfg;

            d[0] = GPAIR(b0, 0, 1, bg0, xf0);
            d[1] = GPAIR(b0, 2, 3, bg0, xf0);
            d[2] = GPAIR(b0, 4, 5, bg0, xf0);
            d[3] = GPAIR(b0, 6, 7, bg0, xf0);

            d[4] = GPAIR(b1, 0, 1, bg1, xf1);
            d[5] = GPAIR(b1, 2, 3, bg1, xf1);
            d[6] = GPAIR(b1, 4, 5, bg1, xf1);
            d[7] = GPAIR(b1, 6, 7, bg1, xf1);

            d += 8;
        }

        /* Trailing odd column (defensive; 100 cols → never taken). */
        if (c < ncols) {
            const uint8_t b0   = s_col_cache[c].glyph ? s_col_cache[c].glyph[gl] : 0u;
            const uint16_t bg0 = s_col_cache[c].bg, xf0 = s_col_cache[c].xorfg;
            d[0] = GPAIR(b0, 0, 1, bg0, xf0);
            d[1] = GPAIR(b0, 2, 3, bg0, xf0);
            d[2] = GPAIR(b0, 4, 5, bg0, xf0);
            d[3] = GPAIR(b0, 6, 7, bg0, xf0);
        }
    }
}

#undef GPAIR
#undef RENDER_MAX_COLS
