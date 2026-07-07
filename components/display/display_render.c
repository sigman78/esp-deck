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
 * Cell buffer — registered once by vterm_init via display_set_text_buffer.
 * Plain statics (DRAM on ESP32, regular BSS on host) so the ISR can reach them.
 * ---------------------------------------------------------------------- */
static DRAM_ATTR const terminal_cell_t *s_cell_buf  = NULL;
static DRAM_ATTR int                    s_cell_cols = 0;
static DRAM_ATTR int                    s_cell_rows = 0;

/* -------------------------------------------------------------------------
 * Overlay buffer — optional second compositing layer.
 * Written by the main task; read by the ISR.  Pointer written last so the ISR
 * never sees a non-NULL pointer with stale cols/rows.
 * ---------------------------------------------------------------------- */
static DRAM_ATTR display_overlay_cell_t *s_overlay_buf  = NULL;
static DRAM_ATTR int                     s_overlay_cols = 0;
static DRAM_ATTR int                     s_overlay_rows = 0;
static DRAM_ATTR color_t                 s_overlay_fg   = COLOR_BLACK;
static DRAM_ATTR color_t                 s_overlay_bg   = COLOR_CYAN;

/* Overlay accent palette (index 0 is a sentinel → use s_overlay_fg). Kept in
 * DRAM so the bounce-buffer ISR can read it without touching flash. */
/* Classic VGA bright-16 palette (the iconic DOS text-mode colors). */
static DRAM_ATTR const color_t s_overlay_pal[OVERLAY_PAL_SIZE] = {
    0,                        /* 0: default → replaced by s_overlay_fg */
    RGB565( 85, 255,  85),    /* 1 green   (VGA bright green)   */
    RGB565( 85, 255, 255),    /* 2 cyan    (VGA bright cyan)    */
    RGB565(255,  85, 255),    /* 3 magenta (VGA bright magenta) */
    RGB565(255, 255,  85),    /* 4 amber   (VGA yellow)         */
    RGB565(255,  85,  85),    /* 5 red     (VGA bright red)     */
    RGB565( 85,  85, 255),    /* 6 blue    (VGA bright blue)    */
    RGB565(255, 255, 255),    /* 7 white   (VGA white)          */
};

/* -------------------------------------------------------------------------
 * Cursor state — updated by terminal via display_set_cursor().
 * Blink is driven internally by a frame counter; ~2 Hz at 60 fps.
 * ---------------------------------------------------------------------- */
#define CURSOR_BLINK_FRAMES  15   /* toggle every 15 frames ≈ 250 ms at 60 fps */

static DRAM_ATTR int           s_cursor_x       = 0;
static DRAM_ATTR int           s_cursor_y       = 0;
static DRAM_ATTR cursor_mode_t s_cursor_mode    = CURSOR_NONE;
static DRAM_ATTR uint32_t      s_blink_count    = 0;
static DRAM_ATTR bool          s_cursor_visible = true;

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
    uint8_t        underline; /* draw fg bar on the last two scanlines    */
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
color_t IRAM_ATTR display_ansi_to_rgb565(uint8_t ansi_color)
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

void display_set_cursor(int x, int y, cursor_mode_t mode)
{
    if (x != s_cursor_x || y != s_cursor_y) {
        s_blink_count    = 0;
        s_cursor_visible = true;
    }
    s_cursor_x    = x;
    s_cursor_y    = y;
    s_cursor_mode = mode;
}

void display_set_overlay_buffer(display_overlay_cell_t *buf, int cols, int rows)
{
    s_overlay_cols = cols;
    s_overlay_rows = rows;
    s_overlay_buf  = buf;   /* written last — atomic 32-bit store; ISR reads after */
}

void display_set_overlay_colors(color_t fg, color_t bg)
{
    s_overlay_fg = fg;
    s_overlay_bg = bg;
}

void display_get_text_size(int *cols, int *rows)
{
    if (cols) *cols = s_cell_cols;
    if (rows) *rows = s_cell_rows;
}

/* -------------------------------------------------------------------------
 * Terminal BELL → screen shake. A visual bell for a device with no speaker:
 * display_bell() arms a decaying vertical jitter; the ISR advances it once per
 * frame and, while active, renders each band shifted (render_shaken_band()).
 * The normal (unshaken) render path below is left untouched.
 * ---------------------------------------------------------------------- */
static DRAM_ATTR volatile int s_bell_frames = 0;   /* frames of shake remaining */
static DRAM_ATTR int          s_shake_dy    = 0;    /* current vertical offset px */

void display_bell(void) { s_bell_frames = 14; }     /* ~230 ms at 60 fps */

static IRAM_ATTR void advance_shake(void)
{
    if (s_bell_frames > 0) {
        int amp = (s_bell_frames * 6) / 14;              /* 6 px → 0, decaying   */
        s_shake_dy = (s_bell_frames & 1) ? amp : -amp;   /* alternate up/down     */
        s_bell_frames--;
    } else {
        s_shake_dy = 0;
    }
}

/* Fill s_col_cache with the resolved fg/bg/glyph for character row @p cr. */
static IRAM_ATTR void build_row_cache(int cr)
{
    const terminal_cell_t *row_cells = s_cell_buf + cr * s_cell_cols;
    const int ncols = s_cell_cols;
    const display_overlay_cell_t *ov_row =
        (s_overlay_buf && cr < s_overlay_rows)
        ? (s_overlay_buf + cr * s_overlay_cols) : NULL;

    for (int c = 0; c < ncols; c++) {
        color_t fg, bg;
        const uint8_t *glyph;
        uint8_t underline = 0;

        const uint8_t  ov_attrs = (ov_row && c < s_overlay_cols) ? ov_row[c].attrs : 0;
        const uint16_t ov_cp    = (ov_row && c < s_overlay_cols) ? ov_row[c].cp    : 0;
        const uint8_t  ov_color = (ov_row && c < s_overlay_cols) ? ov_row[c].color : 0;

        if (ov_cp != 0) {
            fg = ov_color ? s_overlay_pal[ov_color] : s_overlay_fg;
            bg = s_overlay_bg;
            if (ov_attrs & OVERLAY_ATTR_INVERSE) { color_t t = fg; fg = bg; bg = t; }
            glyph = font_get_glyph(ov_cp);
        } else {
            const terminal_cell_t *cell = &row_cells[c];
            fg = cell->fg_color;
            bg = cell->bg_color;
            if (cell->attrs & ATTR_REVERSE) { color_t t = fg; fg = bg; bg = t; }
            underline = cell->attrs & ATTR_UNDERLINE;
            glyph = font_get_glyph(cell->cp);
            if (ov_attrs & OVERLAY_ATTR_DIM) {
                fg = (color_t)((fg >> 1) & 0x7BEFu);
                bg = (color_t)((bg >> 1) & 0x7BEFu);
            }
        }
        s_col_cache[c].glyph     = glyph;
        s_col_cache[c].bg        = bg;
        s_col_cache[c].xorfg     = (uint16_t)(fg ^ bg);
        s_col_cache[c].underline = underline;
    }
}

/* Render one scanline (glyph row @p gl) from s_col_cache. */
static IRAM_ATTR void render_scanline(uint32_t *d, int gl)
{
    const int ncols = s_cell_cols;
    const uint8_t g  = (uint8_t)gl;
    int c = 0;
    for (; c + 1 < ncols; c += 2) {
        const uint8_t b0 = s_col_cache[c    ].glyph ? s_col_cache[c    ].glyph[g] : 0u;
        const uint8_t b1 = s_col_cache[c + 1].glyph ? s_col_cache[c + 1].glyph[g] : 0u;
        const uint16_t bg0 = s_col_cache[c    ].bg, xf0 = s_col_cache[c    ].xorfg;
        const uint16_t bg1 = s_col_cache[c + 1].bg, xf1 = s_col_cache[c + 1].xorfg;
        d[0] = GPAIR(b0,0,1,bg0,xf0); d[1] = GPAIR(b0,2,3,bg0,xf0);
        d[2] = GPAIR(b0,4,5,bg0,xf0); d[3] = GPAIR(b0,6,7,bg0,xf0);
        d[4] = GPAIR(b1,0,1,bg1,xf1); d[5] = GPAIR(b1,2,3,bg1,xf1);
        d[6] = GPAIR(b1,4,5,bg1,xf1); d[7] = GPAIR(b1,6,7,bg1,xf1);
        d += 8;
    }
    if (c < ncols) {
        const uint8_t b0 = s_col_cache[c].glyph ? s_col_cache[c].glyph[g] : 0u;
        const uint16_t bg0 = s_col_cache[c].bg, xf0 = s_col_cache[c].xorfg;
        d[0] = GPAIR(b0,0,1,bg0,xf0); d[1] = GPAIR(b0,2,3,bg0,xf0);
        d[2] = GPAIR(b0,4,5,bg0,xf0); d[3] = GPAIR(b0,6,7,bg0,xf0);
    }
}

/* Shake path: render a band with the whole picture shifted by s_shake_dy px,
 * exposing black where the shift runs past the text area. Only used during a
 * bell, so the per-scanline cache rebuild on a row boundary is fine. */
static IRAM_ATTR void render_shaken_band(color_t *dst, int start_scan, int num_scans)
{
    int last_row = -999;
    for (int n = 0; n < num_scans; n++) {
        uint32_t *d = (uint32_t *)(dst + (unsigned)n * DISPLAY_WIDTH);
        int src_scan = start_scan + n - s_shake_dy;
        int src_row  = (src_scan >= 0) ? src_scan / FONT_HEIGHT : -1;
        if (src_row < 0 || src_row >= s_cell_rows) {
            for (int i = 0; i < DISPLAY_WIDTH / 2; i++) d[i] = 0;   /* black */
            continue;
        }
        if (src_row != last_row) { build_row_cache(src_row); last_row = src_row; }
        render_scanline(d, src_scan - src_row * FONT_HEIGHT);
    }
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

    /* Tick blink counter once per full frame (char_row 0 = start of new frame). */
    if (char_row == 0) {
        if (++s_blink_count >= CURSOR_BLINK_FRAMES) {
            s_blink_count    = 0;
            s_cursor_visible = !s_cursor_visible;
        }
        advance_shake();
    }

    /* Bell shake active — render this band shifted and skip the fast path. */
    if (s_shake_dy != 0 && char_row < s_cell_rows) {
        render_shaken_band(dst, pos_px / DISPLAY_WIDTH, num_scans);
        return;
    }

    /* Below the text area — fill black. */
    if (char_row >= s_cell_rows) {
        uint32_t *p = (uint32_t *)dst;
        int words = n_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        return;
    }

    /* Build the per-column cache for this character row (shared with the
     * bell-shake path). */
    build_row_cache(char_row);
    const int ncols = s_cell_cols;

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

    /* ------------------------------------------------------------------
     * Underline post-pass — force fg on the last two scanlines of any
     * underlined column. Touches only flagged columns, off the hot loop.
     * ------------------------------------------------------------------ */
    if (num_scans >= 2) {
        const int ul_first = num_scans - 2;
        for (int c = 0; c < ncols; c++) {
            if (!s_col_cache[c].underline) continue;
            const uint16_t fg = (uint16_t)(s_col_cache[c].bg ^ s_col_cache[c].xorfg);
            const uint32_t fg2 = (uint32_t)fg | ((uint32_t)fg << 16);
            for (int n = ul_first; n < num_scans; n++) {
                uint32_t *p = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH
                                           + c * FONT_WIDTH);
                p[0] = fg2; p[1] = fg2; p[2] = fg2; p[3] = fg2;
            }
        }
    }

    /* ------------------------------------------------------------------
     * Cursor overlay — XOR every pixel in the cursor region with
     * 0xFFFFFFFF, inverting all bits and guaranteeing contrast.
     * FONT_WIDTH=8 px × 2 B = 16 B = 4 × uint32_t per scanline.
     * Alignment is guaranteed: dst_base is 4-byte aligned,
     * DISPLAY_WIDTH×sizeof(color_t)=1600 B and cx×16 B are both ×4.
     * ------------------------------------------------------------------ */
    if (s_cursor_mode != CURSOR_NONE && s_cursor_visible &&
            s_cursor_y == char_row && s_cursor_x >= 0 && s_cursor_x < ncols) {

        const int cx       = s_cursor_x;
        const int px_start = cx * FONT_WIDTH;

        if (s_cursor_mode == CURSOR_UNDERSCORE) {
            const int first = num_scans >= 2 ? num_scans - 2 : 0;
            for (int n = first; n < num_scans; n++) {
                uint32_t *p = (uint32_t *)(dst_base + n * DISPLAY_WIDTH + px_start);
                p[0] ^= 0xFFFFFFFFu;
                p[1] ^= 0xFFFFFFFFu;
                p[2] ^= 0xFFFFFFFFu;
                p[3] ^= 0xFFFFFFFFu;
            }
        } else { /* CURSOR_BLOCK */
            for (int n = 0; n < num_scans; n++) {
                uint32_t *p = (uint32_t *)(dst_base + n * DISPLAY_WIDTH + px_start);
                p[0] ^= 0xFFFFFFFFu;
                p[1] ^= 0xFFFFFFFFu;
                p[2] ^= 0xFFFFFFFFu;
                p[3] ^= 0xFFFFFFFFu;
            }
        }
    }
}

#undef GPAIR
#undef RENDER_MAX_COLS
