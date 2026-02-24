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
 * Get font glyph bitmap
 *
 * @param ch Character
 * @return Pointer to 16-byte glyph bitmap
 */
const uint8_t* font_get_glyph(char ch);

#endif // FONT_H
