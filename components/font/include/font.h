/*
 * Font rendering interface
 */

#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include "esp_attr.h"

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/**
 * Initialize font system
 */
void font_init(void);

/**
 * Get font glyph bitmap — IRAM_ATTR, safe to call from ISR.
 *
 * @param ch Character
 * @return Pointer to 16-byte glyph bitmap (in DRAM)
 */
const uint8_t* IRAM_ATTR font_get_glyph(char ch);

#endif // FONT_H
