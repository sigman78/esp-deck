/*
 * LCD Driver for Waveshare ESP32-S3-Touch-LCD-7
 * 7-inch 800x480 RGB parallel interface
 *
 * Compatible with ESP-IDF v5.1+
 */

#include "display.h"
#include "font.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "lcd_driver";

// Forward declarations
static IRAM_ATTR bool on_bounce_empty(esp_lcd_panel_handle_t panel,
                                      void *buf,
                                      int   pos_px,
                                      int   len_bytes,
                                      void *user_ctx);
color_t IRAM_ATTR ansi_to_rgb565(uint8_t ansi_color);

// Backlight GPIO
#define PIN_NUM_BK_LIGHT  2

// Static variables
static esp_lcd_panel_handle_t panel_handle = NULL;
static color_t *bounce_buffer = NULL;

// Cell buffer shared with the terminal — written by the terminal task,
// read by the on_bounce_empty ISR.  Pointers and dimensions are plain
// statics (already in DRAM), so the ISR can access them without issues.
static const terminal_cell_t *s_cell_buf  = NULL;
static int                    s_cell_cols = 0;
static int                    s_cell_rows = 0;

/*
 * Per-column rendering cache populated at the start of each on_bounce_empty
 * call.  Static (not stack) to avoid blowing the ISR stack.  Safe because
 * on_bounce_empty is non-reentrant for a single LCD panel.
 *
 * DRAM_ATTR ensures the cache is in internal SRAM, always reachable from ISR
 * without going through the Flash cache.
 */
#define RENDER_MAX_COLS  (DISPLAY_WIDTH / FONT_WIDTH)   /* 100 */

static DRAM_ATTR struct {
    const uint8_t *glyph;   /* 16-byte bitmap in DRAM (from terminus8x16) */
    uint16_t       bg;      /* background colour RGB565                    */
    uint16_t       xorfg;   /* bg ^ fg — XOR in to switch bg→fg per bit   */
} s_col_cache[RENDER_MAX_COLS];

/*
 * Branchless pixel-pair → 32-bit word (little-endian, 2×RGB565).
 *
 * Glyph byte `gb`, pixel positions p0 (left, bit MSB side) and p1 (right):
 *   bit   = (gb >> (7-p)) & 1
 *   mask  = 0xFFFF if bit=1, 0x0000 if bit=0  →  (uint16_t)(0u - bit)
 *   pixel = bg ^ (xorfg & mask)          bg when bit=0, fg when bit=1
 *
 * Left pixel goes into the low 16 bits; right pixel into the high 16 bits
 * (correct for little-endian memory layout expected by the RGB panel).
 */
#define GPAIR(gb, p0, p1, bg_v, xor_v)                                          \
    (   (uint32_t)((uint16_t)((bg_v) ^ ((xor_v) &                               \
            (uint16_t)(0u - (((unsigned)(gb) >> (7u - (p0))) & 1u)))))           \
    |  ((uint32_t)((uint16_t)((bg_v) ^ ((xor_v) &                               \
            (uint16_t)(0u - (((unsigned)(gb) >> (7u - (p1))) & 1u))))) << 16) )

/**
 * Bounce-buffer fill callback — ISR context.
 *
 * Assumptions (guaranteed by BOUNCE_BUFFER_HEIGHT == FONT_HEIGHT and the
 * display height being an exact multiple of FONT_HEIGHT):
 *   • buf is 32-bit aligned
 *   • pos_px is always a multiple of DISPLAY_WIDTH (scanline-aligned)
 *   • len_bytes covers an exact integer number of full scanlines
 *   • The buffer always starts on a character-row boundary, so
 *     glyph_line = scanline_index_within_buffer  (no modulo needed)
 */
static IRAM_ATTR bool on_bounce_empty(esp_lcd_panel_handle_t panel,
                                      void *buf,
                                      int   pos_px,
                                      int   len_bytes,
                                      void *user_ctx)
{
    if (!buf) return false;

    /* Guard: cell buffer not yet registered — fill black. */
    if (!s_cell_buf || s_cell_cols <= 0 || s_cell_rows <= 0) {
        uint32_t *p = (uint32_t *)buf;
        int words = len_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        return false;
    }

    const int start_scan = pos_px / DISPLAY_WIDTH;
    const int num_scans  = (len_bytes >> 1) / DISPLAY_WIDTH;  /* len/2 = pixels */
    const int char_row   = start_scan / FONT_HEIGHT;

    /* Below the text area — fill black. */
    if (char_row >= s_cell_rows) {
        uint32_t *p = (uint32_t *)buf;
        int words = len_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        return false;
    }

    /* ------------------------------------------------------------------
     * Build per-column cache for this character row.
     * ansi_to_rgb565 and font_get_glyph are both IRAM_ATTR; their data
     * (palette, glyph arrays, range table) is DRAM_ATTR — no Flash access.
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
        s_col_cache[c].xorfg = fg ^ bg;
    }

    /* ------------------------------------------------------------------
     * Render scanlines.
     *
     * Because BOUNCE_BUFFER_HEIGHT == FONT_HEIGHT and the buffer is
     * always char-row aligned, the glyph scanline index equals the
     * scanline's index within the bounce buffer (n).
     *
     * Inner loop processes two adjacent columns per iteration → 16 pixels
     * → 8 × uint32_t writes, all naturally 32-bit aligned.
     * ------------------------------------------------------------------ */
    color_t *dst_base = (color_t *)buf;

    for (int n = 0; n < num_scans; n++) {
        const uint8_t gl = (uint8_t)n;          /* glyph scanline 0-15 */
        uint32_t *d = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH);

        int c = 0;
        for (; c + 1 < ncols; c += 2) {
            const uint8_t b0 = s_col_cache[c    ].glyph ? s_col_cache[c    ].glyph[gl] : 0u;
            const uint8_t b1 = s_col_cache[c + 1].glyph ? s_col_cache[c + 1].glyph[gl] : 0u;
            const uint16_t bg0 = s_col_cache[c    ].bg,  xf0 = s_col_cache[c    ].xorfg;
            const uint16_t bg1 = s_col_cache[c + 1].bg,  xf1 = s_col_cache[c + 1].xorfg;

            /* Column c — 8 pixels as 4 × uint32_t, unrolled */
            d[0] = GPAIR(b0, 0, 1, bg0, xf0);
            d[1] = GPAIR(b0, 2, 3, bg0, xf0);
            d[2] = GPAIR(b0, 4, 5, bg0, xf0);
            d[3] = GPAIR(b0, 6, 7, bg0, xf0);

            /* Column c+1 — 8 pixels as 4 × uint32_t, unrolled */
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

    return false;
}

#undef GPAIR
#undef RENDER_MAX_COLS

/**
 * Initialize backlight control GPIO
 */
static esp_err_t init_backlight(void)
{
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    // Turn on backlight (active high)
    gpio_set_level(PIN_NUM_BK_LIGHT, 1);

    ESP_LOGI(TAG, "Backlight initialized (GPIO %d)", PIN_NUM_BK_LIGHT);
    return ESP_OK;
}

/**
 * Initialize RGB LCD panel
 */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing RGB LCD panel (bounce buffer mode)");

    // Initialize backlight
    init_backlight();

    // RGB panel configuration (no framebuffer, bounce buffer mode)
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = DISPLAY_WIDTH,
            .v_res = DISPLAY_HEIGHT,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 0,
        .flags.no_fb = 1,
        .bounce_buffer_size_px = BOUNCE_BUFFER_SIZE,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40},
    };

    // Create RGB panel
    ESP_LOGI(TAG, "Creating RGB panel...");
    esp_err_t ret = esp_lcd_new_rgb_panel(&panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RGB panel: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "RGB panel created successfully");

    ESP_LOGI(TAG, "Resetting panel...");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));

    // Register bounce buffer callback
    ESP_LOGI(TAG, "Registering bounce buffer callback...");
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_bounce_empty = on_bounce_empty
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL));
    ESP_LOGI(TAG, "Callback registered");

    // Allocate bounce buffer in SRAM for best performance
    bounce_buffer = heap_caps_malloc(BOUNCE_BUFFER_SIZE * sizeof(color_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!bounce_buffer) {
        ESP_LOGE(TAG, "Failed to allocate bounce buffer");
        return ESP_ERR_NO_MEM;
    }

    // Initialize panel
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    ESP_LOGI(TAG, "LCD initialized: %dx%d, bounce buffer %d bytes",
             DISPLAY_WIDTH, DISPLAY_HEIGHT, BOUNCE_BUFFER_SIZE * sizeof(color_t));

    return ESP_OK;
}

/**
 * Get LCD panel handle
 */
esp_lcd_panel_handle_t display_get_panel(void)
{
    return panel_handle;
}

/**
 * Get bounce buffer pointer — IRAM_ATTR so the ISR can call it safely.
 */
color_t* IRAM_ATTR display_get_bounce_buffer(void)
{
    return bounce_buffer;
}

/**
 * Register the terminal cell buffer for ISR rendering.
 */
void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows)
{
    s_cell_buf  = buf;
    s_cell_cols = cols;
    s_cell_rows = rows;
    ESP_LOGI(TAG, "Text buffer registered: %dx%d cells @ %p", cols, rows, buf);
}

/**
 * Set backlight brightness
 */
esp_err_t display_set_backlight(uint8_t brightness)
{
    // Simple on/off for now
    // TODO: Implement PWM for gradual brightness control
    //gpio_set_level(PIN_NUM_BK_LIGHT, brightness > 0 ? LCD_BK_LIGHT_ON_LEVEL : LCD_BK_LIGHT_OFF_LEVEL);
    return ESP_OK;
}

/**
 * Convert ANSI 256-color to RGB565.
 * IRAM_ATTR: callable from on_bounce_empty ISR.
 * The palette table is DRAM_ATTR so it is reachable without Flash cache.
 */
color_t IRAM_ATTR ansi_to_rgb565(uint8_t ansi_color)
{
    static DRAM_ATTR const color_t ansi_palette[256] = {
        // 0-15: Standard colors
        RGB565(0, 0, 0),       // 0: Black
        RGB565(128, 0, 0),     // 1: Red
        RGB565(0, 128, 0),     // 2: Green
        RGB565(128, 128, 0),   // 3: Yellow
        RGB565(0, 0, 128),     // 4: Blue
        RGB565(128, 0, 128),   // 5: Magenta
        RGB565(0, 128, 128),   // 6: Cyan
        RGB565(192, 192, 192), // 7: White
        RGB565(128, 128, 128), // 8: Bright Black (Gray)
        RGB565(255, 0, 0),     // 9: Bright Red
        RGB565(0, 255, 0),     // 10: Bright Green
        RGB565(255, 255, 0),   // 11: Bright Yellow
        RGB565(0, 0, 255),     // 12: Bright Blue
        RGB565(255, 0, 255),   // 13: Bright Magenta
        RGB565(0, 255, 255),   // 14: Bright Cyan
        RGB565(255, 255, 255), // 15: Bright White
        // 16-231: 6x6x6 RGB cube (simplified)
        // 232-255: Grayscale (simplified)
        // TODO: Complete palette implementation
    };

    if (ansi_color < 16) {
        return ansi_palette[ansi_color];
    }

    // 216 color cube (16-231)
    if (ansi_color >= 16 && ansi_color <= 231) {
        uint8_t idx = ansi_color - 16;
        uint8_t r = (idx / 36) * 51;
        uint8_t g = ((idx / 6) % 6) * 51;
        uint8_t b = (idx % 6) * 51;
        return RGB565(r, g, b);
    }

    // Grayscale (232-255)
    if (ansi_color >= 232) {
        uint8_t gray = 8 + (ansi_color - 232) * 10;
        return RGB565(gray, gray, gray);
    }

    return COLOR_WHITE; // Fallback
}
