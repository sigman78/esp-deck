/*
 * sim/main.c — PC simulator entry point.
 *
 * Initialises SDL2, font, display (SDL backend), and terminal, then runs
 * a simple event loop that calls display_render_frame() each iteration.
 */

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>

#include "display.h"
#include "font.h"
#include "terminal.h"

/* Terminal dimensions: 800/8 = 100 cols, 480/16 = 30 rows */
#define SIM_COLS  100
#define SIM_ROWS   30

static void show_splash(void)
{
    terminal_clear();

    /* Title bar */
    terminal_set_color(0, 10);   /* black on bright-green */
    terminal_print(" unbreezy cyberdeck — PC simulator                                                   ");
    terminal_set_color(7, 0);    /* white on black */
    terminal_print("\r\n");

    terminal_set_color(10, 0);   /* bright green */
    terminal_print("\r\n  unbreezy v0.1\r\n");
    terminal_set_color(7, 0);
    terminal_print("  ESP32-S3 cyberdeck terminal — SDL2 simulator\r\n\r\n");

    terminal_set_color(14, 0);   /* bright cyan */
    terminal_print("  Display:   ");
    terminal_set_color(7, 0);
    terminal_print("800x480 RGB565  (Waveshare ESP32-S3-Touch-LCD-7)\r\n");

    terminal_set_color(14, 0);
    terminal_print("  Font:      ");
    terminal_set_color(7, 0);
    terminal_print("Terminus 8x16  (100x30 character grid)\r\n");

    terminal_set_color(14, 0);
    terminal_print("  Renderer:  ");
    terminal_set_color(7, 0);
    terminal_print("display_render_chunk()  (shared ISR/SDL2 path)\r\n\r\n");

    /* Colour palette swatch */
    terminal_set_color(7, 0);
    terminal_print("  ANSI-16 palette:\r\n  ");
    for (int i = 0; i < 16; i++) {
        terminal_set_color(0, (uint8_t)i);
        terminal_print("  ");
    }
    terminal_set_color(7, 0);
    terminal_print("\r\n\r\n");

    /* Sample text in several colours */
    const char *lorem =
        "  The quick brown fox jumps over the lazy dog. 0123456789\r\n"
        "  !@#$%^&*()_+-=[]{}|;':\",./<>?  ~`\r\n\r\n";

    terminal_set_color(9,  0); terminal_print(lorem);
    terminal_set_color(10, 0); terminal_print(lorem);
    terminal_set_color(11, 0); terminal_print(lorem);
    terminal_set_color(12, 0); terminal_print(lorem);

    terminal_set_color(7, 0);
    terminal_print("\r\n  Press Escape or close the window to exit.\r\n");
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* SDL2 init — video only; audio/joystick not needed. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Subsystem initialisation order matters:
     *   font_init  → no dependencies
     *   display_init → creates SDL window, renderer, texture
     *   terminal_init → allocates cell buffer, calls display_set_text_buffer
     */
    font_init();

    if (display_init() != ESP_OK) {
        fprintf(stderr, "display_init failed\n");
        SDL_Quit();
        return 1;
    }

    if (terminal_init(SIM_COLS, SIM_ROWS) != ESP_OK) {
        fprintf(stderr, "terminal_init failed\n");
        SDL_Quit();
        return 1;
    }

    show_splash();

    /* -----------------------------------------------------------------------
     * Main event loop — ~60 fps
     * -------------------------------------------------------------------- */
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        running = false;
                        break;
                    default:
                        break;
                }
            }
        }

        display_render_frame();
        SDL_Delay(16);   /* ~60 fps */
    }

    SDL_Quit();
    return 0;
}
