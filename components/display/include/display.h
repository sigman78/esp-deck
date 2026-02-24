/*
 * Display driver interface for Waveshare ESP32-S3-Touch-LCD-7
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

// Display dimensions
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480

// Bounce buffer configuration (one character row at a time)
#define BOUNCE_BUFFER_HEIGHT  16  // One character row (8x16 font)
#define BOUNCE_BUFFER_SIZE    (DISPLAY_WIDTH * BOUNCE_BUFFER_HEIGHT)

// Color format (RGB565)
typedef uint16_t color_t;

// RGB565 color macros
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_RED       RGB565(255, 0, 0)
#define COLOR_GREEN     RGB565(0, 255, 0)
#define COLOR_BLUE      RGB565(0, 0, 255)
#define COLOR_YELLOW    RGB565(255, 255, 0)
#define COLOR_CYAN      RGB565(0, 255, 255)
#define COLOR_MAGENTA   RGB565(255, 0, 255)

/**
 * Initialize display driver
 */
esp_err_t display_init(void);

/**
 * Get LCD panel handle
 */
esp_lcd_panel_handle_t display_get_panel(void);

/**
 * Get bounce buffer pointer (for rendering)
 */
color_t* display_get_bounce_buffer(void);

/**
 * Transfer bounce buffer to specific LCD row
 *
 * @param row_start Y coordinate (0-479)
 * @param row_count Number of rows to transfer (typically 16 for one char row)
 */
esp_err_t display_flush_row(int row_start, int row_count);

/**
 * Clear entire display (fill with color)
 * Uses bounce buffer to clear screen efficiently
 */
esp_err_t display_clear_screen(color_t color);

/**
 * Set backlight brightness (0-100%)
 */
esp_err_t display_set_backlight(uint8_t brightness);

/**
 * Convert ANSI 256-color to RGB565
 */
color_t ansi_to_rgb565(uint8_t ansi_color);

/*
 * Note: Direct pixel/rectangle drawing functions removed.
 * With bounce buffer rendering, all drawing happens through:
 * 1. Get bounce buffer with display_get_bounce_buffer()
 * 2. Render to bounce buffer
 * 3. Flush with display_flush_row()
 */

#endif // DISPLAY_H
