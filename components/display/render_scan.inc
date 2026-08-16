/*
 * render_scan.inc — the HOT band-scan loop, "templatized" the C way.
 * #include this file once per font size (see render_scan.c) after defining:
 *
 *   SCAN_W    cell width in pixels (integer literal: 8, 10, 12)
 *   SCAN_ROW  glyph row type: uint8_t (W <= 8) or uint16_t
 *   SCAN_FN   name of the emitted function
 *
 * Each instantiation is a real, named, debuggable function in which the
 * width, row type, glyph stride and stores-per-column are compile-time
 * constants — the per-pixel path matches a fixed-size build (~13
 * cycles/pixel measured; this loop IS the ISR's render floor, so treat
 * any edit as hot-path work and re-run the CYBERDECK_BENCH_STRESS
 * numbers).
 *
 * Structure notes, both load-bearing at -Os:
 *   - columns go in PAIRS with hoisted row/color loads (load latency
 *     hides behind the first column's stores);
 *   - the pixel loops need RENDER_UNROLL — -Os never unrolls on its own,
 *     and a rolled emit loop costs a compare+branch per pixel pair.
 *
 * The glyph stride is 2*W*sizeof(SCAN_ROW): every cell height is exactly
 * 2*W for the sizes we ship, so the stride folds to a constant here (the
 * generic g_rs.gb is not needed in this loop).
 */

static IRAM_ATTR void SCAN_FN(const scan_ctx_t *cx)
{
    enum { W = SCAN_W, CW = SCAN_W / 2 };   /* CW: uint32 stores per column */
    const int ncols = cx->ncols;
    const uint8_t *const rows = cx->rows;

    for (int n = 0; n < cx->num_scans; n++) {
        const int gl = cx->glyph_row0 + n;
        /* CRT scanlines: odd scanlines read the pre-dimmed color variant.
         * Bands start on even scanlines, so n's parity is global parity. */
        const int sel = n & cx->scan_on & 1;
        const uint16_t *bgv = cx->bg[sel];
        const uint16_t *xfv = cx->xf[sel];
        uint32_t *d = (uint32_t *)(cx->dst + (unsigned)n * DISPLAY_WIDTH);

        int c = 0;
        for (; c + 1 < ncols; c += 2) {
            const SCAN_ROW *g0 = (const SCAN_ROW *)(rows
                    + (size_t)(c    ) * (2u * W * sizeof(SCAN_ROW)));
            const SCAN_ROW *g1 = (const SCAN_ROW *)(rows
                    + (size_t)(c + 1) * (2u * W * sizeof(SCAN_ROW)));
            const unsigned b0  = g0[gl];
            const unsigned b1  = g1[gl];
            const uint16_t bg0 = bgv[c    ], xf0 = xfv[c    ];
            const uint16_t bg1 = bgv[c + 1], xf1 = xfv[c + 1];

            RENDER_UNROLL
            for (int p = 0; p < W; p += 2)
                d[p >> 1] = scan_gpair(b0, W, p, bg0, xf0);
            RENDER_UNROLL
            for (int p = 0; p < W; p += 2)
                d[CW + (p >> 1)] = scan_gpair(b1, W, p, bg1, xf1);
            d += 2 * CW;
        }

        /* Trailing odd column (defensive; all shipped grids are even). */
        if (c < ncols) {
            const SCAN_ROW *g0 = (const SCAN_ROW *)(rows
                    + (size_t)c * (2u * W * sizeof(SCAN_ROW)));
            const unsigned b0 = g0[gl];
            RENDER_UNROLL
            for (int p = 0; p < W; p += 2)
                d[p >> 1] = scan_gpair(b0, W, p, bgv[c], xfv[c]);
            d += CW;
        }

        /* Blank the margin right of the text area (black). */
        for (int i = 0; i < cx->margin_words; i++)
            d[i] = 0;
    }
}
