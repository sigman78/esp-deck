/*
 * vterm — VT/ANSI terminal emulator, libtsm backend.
 *
 * tsm_vte parses the byte stream; tsm_screen manages the cell grid.
 * draw_cb() copies each cell into the flat terminal_cell_t buffer
 * that the display ISR reads directly.
 */

#include "vterm.h"
#include "libtsm.h"
#include "display.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#ifndef VTERM_BUF_SIZE
#define VTERM_BUF_SIZE 256
#endif

static const char *TAG = "vterm";

static struct tsm_screen  *s_screen;
static struct tsm_vte     *s_vte;

static terminal_cell_t    *s_buffer;
static int                 s_cols;
static int                 s_rows;

static vterm_response_cb_t s_response_cb;
static void               *s_response_user;

static bool                s_initialized;
static tsm_age_t           s_last_age;    /* 0 = force full redraw */

static char                s_wbuf[VTERM_BUF_SIZE];
static size_t              s_wbuf_len;

/* -------------------------------------------------------------------------
 * libtsm callbacks
 * ---------------------------------------------------------------------- */

static void vte_write_cb(struct tsm_vte *vte,
                         const char *u8, size_t len,
                         void *data)
{
    (void)vte; (void)data;
    if (s_response_cb)
        s_response_cb(u8, len, s_response_user);
}

static int draw_cb(struct tsm_screen *screen,
                   uint64_t id,
                   const uint32_t *ch,
                   size_t len,
                   unsigned int width,
                   unsigned int posx,
                   unsigned int posy,
                   const struct tsm_screen_attr *attr,
                   tsm_age_t age,
                   void *data)
{
    (void)screen; (void)id; (void)width; (void)data;

    if ((int)posx >= s_cols || (int)posy >= s_rows)
        return 0;

    /* age == 0 means age_reset (counter overflow) — always redraw.
     * age <= s_last_age means cell unchanged since last draw — skip. */
    if (age != 0 && age <= s_last_age)
        return 0;

    int idx = (int)posy * s_cols + (int)posx;

    s_buffer[idx].cp = (len > 0 && ch[0] < 0x10000u)
                     ? (uint16_t)ch[0]
                     : 0x0020u;

    /* fccode/bccode 0-15 are ANSI indices.
     * TSM_COLOR_FOREGROUND (16) → 7 (white), TSM_COLOR_BACKGROUND (17) → 0.
     * Negative means use fr/fg/fb RGB; fall back to white/black.           */
    /* libtsm colour path:
     *   fccode/bccode 0-15   → named ANSI colour (palette index)
     *   fccode/bccode == TSM_COLOR_FOREGROUND/BACKGROUND → terminal default
     *   fccode/bccode == -1  → RGB stored in fr/fg/fb or br/bg/bb
     *     (set for all 256-colour indices ≥16 after lookup_color() and for
     *      true-colour SGR 38;2 / 48;2)                                    */
    if (attr->fccode < 0)
        s_buffer[idx].fg_color = RGB565(attr->fr, attr->fg, attr->fb);
    else if (attr->fccode == TSM_COLOR_FOREGROUND)
        s_buffer[idx].fg_color = display_ansi_to_rgb565(7);
    else if (attr->fccode == TSM_COLOR_BACKGROUND)
        s_buffer[idx].fg_color = display_ansi_to_rgb565(0);
    else
        s_buffer[idx].fg_color = display_ansi_to_rgb565((uint8_t)attr->fccode);

    if (attr->bccode < 0)
        s_buffer[idx].bg_color = RGB565(attr->br, attr->bg, attr->bb);
    else if (attr->bccode == TSM_COLOR_FOREGROUND)
        s_buffer[idx].bg_color = display_ansi_to_rgb565(7);
    else if (attr->bccode == TSM_COLOR_BACKGROUND)
        s_buffer[idx].bg_color = display_ansi_to_rgb565(0);
    else
        s_buffer[idx].bg_color = display_ansi_to_rgb565((uint8_t)attr->bccode);

    uint8_t attrs = 0;
    if (attr->bold)      attrs |= ATTR_BOLD;
    if (attr->underline) attrs |= ATTR_UNDERLINE;
    if (attr->inverse)   attrs |= ATTR_REVERSE;
    if (attr->blink)     attrs |= ATTR_BLINK;
    s_buffer[idx].attrs = attrs;

    return 0;
}

static inline void refresh_display(void)
{
    s_last_age = tsm_screen_draw(s_screen, draw_cb, NULL);
    display_set_cursor((int)tsm_screen_get_cursor_x(s_screen),
                       (int)tsm_screen_get_cursor_y(s_screen),
                       CURSOR_BLOCK);
}

static inline void flush_buf(void)
{
    if (s_wbuf_len == 0) return;
    tsm_vte_input(s_vte, s_wbuf, s_wbuf_len);
    s_wbuf_len = 0;
    refresh_display();
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

esp_err_t vterm_init(int cols, int rows)
{
    ESP_LOGI(TAG, "Initializing vterm (%dx%d)", cols, rows);

    s_cols        = cols;
    s_rows        = rows;
    s_initialized = false;

    s_buffer = malloc((size_t)cols * (size_t)rows * sizeof(terminal_cell_t));
    if (!s_buffer) {
        ESP_LOGE(TAG, "Failed to allocate cell buffer");
        return ESP_ERR_NO_MEM;
    }

    color_t def_fg = display_ansi_to_rgb565(7);
    color_t def_bg = display_ansi_to_rgb565(0);
    for (int i = 0; i < cols * rows; i++) {
        s_buffer[i].cp       = 0x0020;
        s_buffer[i].fg_color = def_fg;
        s_buffer[i].bg_color = def_bg;
        s_buffer[i].attrs    = 0;
    }

    if (tsm_screen_new(&s_screen, NULL, NULL) < 0) {
        ESP_LOGE(TAG, "tsm_screen_new failed");
        free(s_buffer);
        return ESP_FAIL;
    }
    tsm_screen_resize(s_screen, (unsigned)cols, (unsigned)rows);

    if (tsm_vte_new(&s_vte, s_screen, vte_write_cb, NULL, NULL, NULL) < 0) {
        ESP_LOGE(TAG, "tsm_vte_new failed");
        tsm_screen_unref(s_screen);
        free(s_buffer);
        return ESP_FAIL;
    }

    s_initialized = true;

    display_set_text_buffer(s_buffer, cols, rows);
    display_set_cursor(0, 0, CURSOR_BLOCK);

    ESP_LOGI(TAG, "vterm ready: %dx%d, %zu bytes",
             cols, rows, (size_t)cols * (size_t)rows * sizeof(terminal_cell_t));
    return ESP_OK;
}

void vterm_write(const char *data, size_t len)
{
    if (!s_initialized || len == 0) return;
    while (len > 0) {
        size_t avail = VTERM_BUF_SIZE - s_wbuf_len;
        size_t scan  = len < avail ? len : avail;
        const char *lf = memchr(data, '\n', scan);
        size_t copy = lf ? (size_t)(lf - data) + 1 : scan;
        memcpy(s_wbuf + s_wbuf_len, data, copy);
        s_wbuf_len += copy;
        data += copy;
        len  -= copy;
        if (lf || s_wbuf_len == VTERM_BUF_SIZE)
            flush_buf();
    }
}

void vterm_write_dir(const char *data, size_t len)
{
    if (!s_initialized) return;
    flush_buf();
    tsm_vte_input(s_vte, data, len);
    refresh_display();
}

void vterm_flush(void)
{
    if (!s_initialized) return;
    flush_buf();
}

void vterm_set_response_cb(vterm_response_cb_t cb, void *user)
{
    s_response_cb   = cb;
    s_response_user = user;
}

void vterm_reset(void)
{
    if (!s_initialized) return;
    flush_buf();
    s_last_age = 0;          /* force full redraw after reset */
    tsm_vte_reset(s_vte);
    refresh_display();
}
