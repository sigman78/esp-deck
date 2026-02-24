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

