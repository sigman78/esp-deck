/*
 * display_render.c — shared rendering core, compiled for BOTH the ESP32
 * target (bounce-buffer ISR) and the PC simulator (SDL2 frame loop).
 * No SDL or esp_lcd headers — only display.h and font.h.
 *
 * This file is the SKELETON: public geometry API, the per-chunk pipeline
 * (in order: frame tick → clip window → cache resolve → band scan →
 * output passes → effect passes) and the optional cycle bench. The work
 * lives in the sibling modules — see render_internal.h for the map.
 */

#include "render_internal.h"
#include "display_render.h"
#include "display_fx_internal.h"
#include "font.h"

/* -------------------------------------------------------------------------
 * Active cell geometry. Chosen once at boot (the font size is a stored
 * setting applied on reboot) and then read by the ISR on every band, so it
 * lives in DRAM rather than behind a call into the font component.
 * ---------------------------------------------------------------------- */
DRAM_ATTR render_geom_t g_rs = {
    .fw   = 8,
    .fh   = 16,
    .band = 16,
    .gb   = 16,   /* font_glyph_bytes() of the active size */
};

int display_band_height(void) { return g_rs.band; }
int display_bounce_px(void)   { return DISPLAY_WIDTH * g_rs.band; }
int display_text_cols(void)   { return DISPLAY_WIDTH  / g_rs.fw; }
int display_text_rows(void)   { return DISPLAY_HEIGHT / g_rs.fh; }

void display_render_set_font(int width, int height)
{
    /* Bands must be even (scanline-parity effects), divide the cell height
     * (never straddle a character row) and divide the panel height. Checked
     * at runtime because the size is no longer a build constant; a bad
     * value keeps the previous geometry instead of tearing in the ISR. */
    const int band = (height > 16) ? (height / 2) : height;

    if (width <= 0 || height <= 0 ||
        (width & 1) || (band & 1) ||
        (height % band) != 0 ||
        (DISPLAY_HEIGHT % height) != 0 ||
        (DISPLAY_HEIGHT % band) != 0 ||
        (DISPLAY_WIDTH / width) > FONT_MAX_COLS) {
        return;
    }

    g_rs.fw   = width;
    g_rs.fh   = height;
    g_rs.band = band;
    g_rs.gb   = (int)font_glyph_bytes();
    render_cache_invalidate();
    render_fx_snapshot();   /* seed g_fx_snap before the first scan */
}

/* ANSI-256 colour → RGB565. IRAM: callable from the ISR (in practice the
 * terminal converts at SGR-parse time; this is not per-pixel work). */
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

static IRAM_ATTR void fill_black(color_t *dst, int n_bytes)
{
    uint32_t *p = (uint32_t *)dst;
    const int words = n_bytes >> 2;
    for (int i = 0; i < words; i++) p[i] = 0;
}

/**
 * Render one horizontal band (one bounce-buffer height) into dst.
 * A band never straddles a character row: it covers either a whole row
 * (8x16) or half of one (10x20, 12x24). pos_px = start scanline ×
 * DISPLAY_WIDTH; n_bytes = band pixel count × sizeof(color_t).
 */
static IRAM_ATTR void render_chunk_body(color_t *dst, int pos_px, int n_bytes)
{
    if (!dst) return;

    /* Cell buffer not yet registered — fill black. */
    if (!render_cache_has_cells()) {
        fill_black(dst, n_bytes);
        return;
    }

    const int fh         = g_rs.fh;
    const int start_scan = pos_px / DISPLAY_WIDTH;
    const int num_scans  = (n_bytes >> 1) / DISPLAY_WIDTH;  /* n_bytes/2 = px */
    const int char_row   = start_scan / fh;
    /* First glyph scanline of this band; non-zero only with sub-row bands.
     * Always a multiple of the (even) band height, so scanline-parity
     * effects stay aligned. */
    const int glyph_row0 = start_scan % fh;

    /* Per-frame counters tick once per frame (scanline 0 = new frame). */
    if (start_scan == 0) {
        render_fx_frame_tick();
        render_cursor_tick();
    }

    /* Below the text area — fill black. */
    if (char_row >= render_cache_text_rows()) {
        fill_black(dst, n_bytes);
        return;
    }

    /* Wipe / collapse transitions clip the visible scanline window. A band
     * fully outside it skips rendering — and must drop the row cache, so a
     * later band can never scan a cache this path skipped rebuilding. */
    fx_clip_t clip;
    render_fx_clip_window(&clip);
    if (render_fx_band_hidden(&clip, start_scan, num_scans)) {
        render_cache_invalidate();
        render_fx_fill_hidden(dst, start_scan, num_scans, &clip);
        return;
    }

    scan_ctx_t cx = {
        .dst        = dst,
        .ncols      = render_cache_text_cols(),
        .num_scans  = num_scans,
        .glyph_row0 = glyph_row0,
        .scan_on    = g_fx_snap.scanlines ? 1 : 0,
    };
    /* Right margin beyond the text area, in uint32 (pixel-pair) words.
     * Zero for 8x16/10x20; 4 words (8 px) for 12x24 (66*12 = 792 < 800). */
    cx.margin_words = (DISPLAY_WIDTH - cx.ncols * g_rs.fw) >> 1;

    /* Decode + resolve this row into the column cache (no-op when the
     * row's first band already built it), then scan and decorate. */
    render_cache_resolve(char_row, cx.scan_on, &cx);

    render_scan_band(&cx);            /* the hot loop — render_scan.inc  */
    render_underline_pass(&cx);
#if OVERLAY_DIM_DITHER
    render_dim_pass(&cx);
#endif
    render_cursor_pass(&cx, char_row);

    render_fx_wobble(dst, start_scan, num_scans);
    render_fx_static(dst, start_scan, num_scans);
    if (clip.active)
        render_fx_clip_apply(dst, start_scan, num_scans, &clip);
    render_fx_bell_tag(dst, char_row, glyph_row0, num_scans);
}

/* -------------------------------------------------------------------------
 * Public entry — optionally wrapped in a cycle-count bench (CONFIG_
 * DISPLAY_ISR_BENCH, default y): per-chunk CPU cycles accumulated in DRAM,
 * drained by display_render_bench_get()/_reset() from any task. ~20
 * cycles/chunk overhead when enabled.
 * ---------------------------------------------------------------------- */
#ifdef CONFIG_DISPLAY_ISR_BENCH
#include "esp_cpu.h"

/* Cycle total must be 64-bit: a 30 s window at ~113k cycles/chunk is within
 * 8% of the u32 wrap, and any longer window overflows. The ISR's two-word
 * add can tear under a cross-core read, so the reader spins until the chunk
 * count is stable around the read. */
static DRAM_ATTR struct {
    volatile uint64_t cyc;
    volatile uint32_t n;
    volatile uint32_t max;
} s_bench = {};

void display_render_bench_get(uint32_t *avg_cycles, uint32_t *max_cycles, uint32_t *chunks)
{
    uint64_t cyc;
    uint32_t n, n2;
    do {
        n   = s_bench.n;
        cyc = s_bench.cyc;
        n2  = s_bench.n;
    } while (n != n2);
    if (avg_cycles) *avg_cycles = n ? (uint32_t)(cyc / n) : 0;
    if (max_cycles) *max_cycles = s_bench.max;
    if (chunks)     *chunks     = n;
}

void display_render_bench_reset(void)
{
    s_bench.cyc = 0;
    s_bench.n   = 0;
    s_bench.max = 0;
}

void IRAM_ATTR display_render_chunk(color_t *dst, int pos_px, int n_bytes)
{
    const uint32_t t0 = esp_cpu_get_cycle_count();
    render_chunk_body(dst, pos_px, n_bytes);
    const uint32_t dt = esp_cpu_get_cycle_count() - t0;
    s_bench.cyc += dt;
    s_bench.n++;
    if (dt > s_bench.max) s_bench.max = dt;
}
#else
void display_render_bench_get(uint32_t *avg_cycles, uint32_t *max_cycles, uint32_t *chunks)
{
    if (avg_cycles) *avg_cycles = 0;
    if (max_cycles) *max_cycles = 0;
    if (chunks)     *chunks     = 0;
}

void display_render_bench_reset(void) { }

void IRAM_ATTR display_render_chunk(color_t *dst, int pos_px, int n_bytes)
{
    render_chunk_body(dst, pos_px, n_bytes);
}
#endif
