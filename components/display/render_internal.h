/*
 * render_internal.h — shared internals of the render core, split across:
 *
 *   display_render.c   public API + state, per-chunk skeleton, bench
 *   render_cache.c     cell/overlay buffers + the per-row column cache
 *   render_scan.c      the HOT per-size band scan + scan-output passes
 *   render_fx_pass.c   effect application (clip, wobble, static, bell)
 *
 * NOT part of the public display API. Everything here is reachable from
 * the bounce-buffer ISR: definitions live in DRAM, functions in IRAM.
 */

#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H

#include "display.h"
#include "display_fx.h"
#include <stdbool.h>

/* Sized for the WIDEST selectable grid (8x16 -> 100). One shared set of
 * caches, not one per size: only one size is ever active; the tail beyond
 * the active grid goes unused. */
#define RENDER_MAX_COLS  FONT_MAX_COLS

/* Overlay DIM scrim style: 0 = 50% brightness per-cell (cheapest ISR path,
 * default), 1 = dithered checkerboard (crisper but adds a per-band post-
 * pass, compiled out when 0). */
#ifndef OVERLAY_DIM_DITHER
#define OVERLAY_DIM_DITHER 0
#endif

/* The project builds at -Os, which unrolls nothing on its own — the hot
 * loop's constant-trip pixel loops need the explicit pragma to match the
 * hand-unrolled codegen this replaced. MSVC (simulator) ignores it; sim
 * performance is irrelevant. */
#if defined(__GNUC__)
#define RENDER_UNROLL       _Pragma("GCC unroll 6")
#define RENDER_FORCE_INLINE __attribute__((always_inline)) static inline
#else
#define RENDER_UNROLL
#define RENDER_FORCE_INLINE static inline
#endif

/* ------------------------------------------------------------------------
 * Active cell geometry — owned by display_render.c (set once at boot via
 * display_render_set_font), read by every module on every band.
 * ---------------------------------------------------------------------- */
typedef struct {
    int fw;      /* cell width                            */
    int fh;      /* cell height                           */
    int band;    /* bounce band height, in scanlines      */
    int gb;      /* glyph stride in the row cache, bytes  */
} render_geom_t;
extern DRAM_ATTR render_geom_t g_rs;

/* ------------------------------------------------------------------------
 * Per-band scan context — filled by the chunk skeleton + cache resolve,
 * consumed by the scan and its output passes. Passing one pointer keeps
 * the cross-file calls register-cheap.
 * ---------------------------------------------------------------------- */
typedef struct {
    color_t        *dst;         /* band origin in the pixel buffer      */
    const uint8_t  *rows;        /* decoded glyph rows, g_rs_gb stride   */
    const uint16_t *bg[2];       /* per-column bg: [0] normal [1] dimmed */
    const uint16_t *xf[2];       /* per-column fg^bg, same variants      */
    const uint8_t  *ul;          /* per-column underline flags           */
#if OVERLAY_DIM_DITHER
    const uint8_t  *dim;         /* per-column scrim flags               */
#endif
    int  ncols;
    int  num_scans;              /* scanlines in this band               */
    int  glyph_row0;             /* first glyph row of the band          */
    int  scan_on;                /* 1 = scanline-dim variant in use      */
    int  margin_words;           /* black pixel-pairs right of the grid  */
    bool any_ul;                 /* any column underlined this row?      */
#if OVERLAY_DIM_DITHER
    bool any_dim;
#endif
} scan_ctx_t;

/* ---- render_cache.c ---------------------------------------------------- */
/* Ensure the column cache holds (char_row, scan_on, current frame); fill
 * the cache pointers + any_* flags of @p cx either way. */
void render_cache_resolve(int char_row, int scan_on, scan_ctx_t *cx);
/* Drop the cache (font change, effect-hidden band skipped a rebuild). */
void render_cache_invalidate(void);
/* Registered cell grid (0 / false before vterm_init registers it). */
int  render_cache_text_rows(void);
int  render_cache_text_cols(void);
bool render_cache_has_cells(void);

/* ---- render_scan.c ----------------------------------------------------- */
void render_scan_band(const scan_ctx_t *cx);        /* HOT per-size loop  */
void render_underline_pass(const scan_ctx_t *cx);
#if OVERLAY_DIM_DITHER
void render_dim_pass(const scan_ctx_t *cx);
#endif
void render_cursor_pass(const scan_ctx_t *cx, int char_row);
void render_cursor_tick(void);                      /* blink, per frame   */

/* ---- render_fx_pass.c -------------------------------------------------- */
/* Wipe/collapse clip window for this frame; valid while `active`. */
typedef struct {
    bool    active;
    int     wv0, wv1;            /* visible scanline range [wv0, wv1)    */
    int     ea0, ea1, eb0, eb1;  /* bright edge line ranges              */
    color_t ecol;                /* edge color                           */
} fx_clip_t;

/* Per-frame snapshot of g_fx_cfg, taken at the frame tick (owned by
 * render_fx_pass.c): every per-row/per-band path reads THIS, never the
 * live config, so an fx change applies at a frame boundary — no mid-frame
 * seam, no torn per-band reads. */
extern DRAM_ATTR display_fx_cfg_t g_fx_snap;
void render_fx_snapshot(void);                      /* refresh g_fx_snap  */

void render_fx_frame_tick(void);                    /* counters + bell    */
void render_fx_clip_window(fx_clip_t *out);
/* Band lies fully outside the visible window? */
bool render_fx_band_hidden(const fx_clip_t *c, int band_y0, int num_scans);
/* Black + edge lines for a fully hidden band. */
void render_fx_fill_hidden(color_t *dst, int band_y0, int num_scans,
                           const fx_clip_t *c);
/* Post-pass for bands straddling the window edge. */
void render_fx_clip_apply(color_t *dst, int band_y0, int num_scans,
                          const fx_clip_t *c);
void render_fx_wobble(color_t *dst, int start_scan, int num_scans);
void render_fx_static(color_t *dst, int start_scan, int num_scans);
void render_fx_bell_tag(color_t *dst, int char_row, int glyph_row0,
                        int num_scans);

#endif /* RENDER_INTERNAL_H */
