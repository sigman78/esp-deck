/*
 * Font rendering interface
 */

#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include "display.h"

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/**
 * Initialize font system
 */
void font_init(void);

/**
 * Draw a single character
 *
 * @param x X position (pixels)
 * @param y Y position (pixels)
 * @param ch Character to draw
 * @param fg Foreground color
 * @param bg Background color
 */
void font_draw_char(int x, int y, char ch, color_t fg, color_t bg);

/**
 * Draw a string
 *
 * @param x X position (pixels)
 * @param y Y position (pixels)
 * @param str String to draw
 * @param fg Foreground color
 * @param bg Background color
 */
void font_draw_string(int x, int y, const char *str, color_t fg, color_t bg);

/**
 * Get font glyph bitmap
 *
 * @param ch Character
 * @return Pointer to 16-byte glyph bitmap
 */
const uint8_t* font_get_glyph(char ch);

#endif // FONT_H
