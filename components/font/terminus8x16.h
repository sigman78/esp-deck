#ifndef TERMINUS8X16_H
#define TERMINUS8X16_H

#include <stdint.h>

// Returns a pointer to a 16-byte array for the requested Unicode codepoint.
// Returns a fallback character (like '?') if the codepoint is empty/missing.
const uint8_t* terminus8x16_get_glyph(uint16_t codepoint);

#endif // TERMINUS8X16_H
