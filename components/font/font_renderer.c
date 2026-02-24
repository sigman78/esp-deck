/*
 * Font renderer implementation
 */

#include "font.h"
#include "terminus8x16.h"
#include "esp_attr.h"
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
 * Get font glyph bitmap — IRAM_ATTR so it is safe to call from the ISR.
 */
const uint8_t* IRAM_ATTR font_get_glyph(uint16_t cp)
{
    return terminus8x16_get_glyph(cp);
}

