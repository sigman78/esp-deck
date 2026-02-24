/*
 * LCD Driver for Waveshare ESP32-S3-Touch-LCD-7
 * 7-inch 800x480 RGB parallel interface
 *
 * Compatible with ESP-IDF v5.1+
 */

#include "display.h"
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
static bool on_bounce_empty(esp_lcd_panel_handle_t panel,
                                       void *buf,
                                       int pos_px,
                                       int len_bytes,
                                       void *user_ctx);

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

/**
 * Callback called when bounce buffer is empty and needs refilling
 * This is called from ISR context, so keep it fast!
 *
 * @param panel Panel handle
 * @param buf Pointer to bounce buffer to fill
 * @param pos_px Position in pixels (y-coordinate * width)
 * @param len_bytes Number of bytes to fill
 * @param user_ctx User context (not used)
 */
static IRAM_ATTR bool on_bounce_empty(esp_lcd_panel_handle_t panel,
                                       void *buf,
                                       int pos_px,
                                       int len_bytes,
                                       void *user_ctx)
{
    // Don't increment counter every time - too slow
    // callback_count++;

    if (!buf) {
        return false;
    }

    // Pre-calculated 32-bit words (two pixels each)
    const uint32_t white_white = 0xFFFFFFFF;
    const uint32_t red_red = ((uint32_t)COLOR_RED << 16) | COLOR_RED;
    const uint32_t white_red = ((uint32_t)COLOR_RED << 16) | COLOR_WHITE;
    const uint32_t red_white = ((uint32_t)COLOR_WHITE << 16) | COLOR_RED;

    uint32_t *pixels32 = (uint32_t *)buf;

    // Calculate starting row and column from pos_px
    int start_row = pos_px / DISPLAY_WIDTH;
    //int start_col = pos_px - (start_row * DISPLAY_WIDTH);  // Faster than modulo

    // assume bounce buffer update is always aligned to start of line and contains exact number of full lines
    int num_lines = (len_bytes / 2) / DISPLAY_WIDTH;

    int i = 0;
    for (int n = 0; n < num_lines; n++) {
        // Calculate current position for this word (2 pixels)
        int row = start_row + n;

        // Row check bit (32-pixel squares vertically)
        int row_check = (row >> 5) & 1;

        for(int dx = 0; dx < DISPLAY_WIDTH / 2; dx++) {
            // Column check bits for the two pixels
            int col_check0 = (dx >> 4) & 1;
            int col_check1 = col_check0;

            // XOR with row to get checkerboard
            int check0 = row_check ^ col_check0;
            int check1 = row_check ^ col_check1;

            // Select the right pre-calculated word
            if (check0 == check1) {
                pixels32[i] = check0 ? white_white : red_red;
            } else if (check0) {
                pixels32[i] = white_red;  // First white, second red
            } else {
                pixels32[i] = red_white;  // First red, second white
            }
            i++;
        }
    }

    return false;  // Return false = no high priority task woken
}

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
