/*
 * Global configuration and constants
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "sdkconfig.h"

// Terminal configuration
#define TERM_WIDTH      CONFIG_TERMINAL_WIDTH
#define TERM_HEIGHT     CONFIG_TERMINAL_HEIGHT
#define SCROLLBACK_SIZE CONFIG_SCROLLBACK_LINES

// Display configuration
#define LCD_H_RES       800
#define LCD_V_RES       480

// Font configuration
#define FONT_WIDTH      8
#define FONT_HEIGHT     16

// Color configuration
#define DEFAULT_FG      CONFIG_DEFAULT_FG_COLOR
#define DEFAULT_BG      CONFIG_DEFAULT_BG_COLOR

// SSH configuration
#define SSH_KEEPALIVE   CONFIG_SSH_KEEPALIVE_INTERVAL

#endif // CONFIG_H
