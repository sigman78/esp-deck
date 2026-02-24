/*
 * Font renderer implementation
 */

#include "font.h"
#include "terminus8x16.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "font_renderer";

/**
 * Initialize font system
 */
void font_init(void)
{
    ESP_LOGI(TAG, "Font system initialized (Terminus 8x16)");
}

/**
 * Get font glyph bitmap
 */
const uint8_t* font_get_glyph(char ch)
{
    // Use the real Terminus font function
    // Convert char to Unicode codepoint (assuming ASCII/Latin-1)
    uint16_t codepoint = (uint8_t)ch;
    return terminus8x16_get_glyph(codepoint);
}

/**
 * Draw a single character to bounce buffer
 * Note: This renders to a relative position in the bounce buffer
 *
 * @param x X position in bounce buffer (0-799)
 * @param y Y position relative to bounce buffer (0-15)
 * @param ch Character to draw
 * @param fg Foreground color
 * @param bg Background color
 */
void font_draw_char(int x, int y, char ch, color_t fg, color_t bg)
{
    const uint8_t *glyph = font_get_glyph(ch);
    color_t *bb = display_get_bounce_buffer();

    if (!bb) return;

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t line = glyph[row];
        int py = y + row;

        // Only render if within bounce buffer bounds
        if (py >= 0 && py < BOUNCE_BUFFER_HEIGHT) {
            for (int col = 0; col < FONT_WIDTH; col++) {
                int px = x + col;

                if (px >= 0 && px < DISPLAY_WIDTH) {
                    color_t color = (line & (0x80 >> col)) ? fg : bg;
                    bb[py * DISPLAY_WIDTH + px] = color;
                }
            }
        }
    }
}

/**
 * Draw a string to bounce buffer
 * Note: With bounce buffer rendering, this function is less useful
 * as terminal typically renders one row at a time
 */
void font_draw_string(int x, int y, const char *str, color_t fg, color_t bg)
{
    int cursor_x = x;

    while (*str) {
        if (*str == '\n') {
            // Newlines not supported in bounce buffer (single row at a time)
            break;
        } else {
            font_draw_char(cursor_x, y, *str, fg, bg);
            cursor_x += FONT_WIDTH;

            // Stop if we exceed display width
            if (cursor_x >= DISPLAY_WIDTH) {
                break;
            }
        }
        str++;
    }
}
