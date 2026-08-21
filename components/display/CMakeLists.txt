include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         display_render.c render_cache.c render_scan.c
                 render_fx_pass.c display_fx.c
    SRCS_DEV     lcd_driver.c
    SRCS_SIM     display_sdl.c
    INCLUDE_DIRS include
    REQUIRES     font
    REQUIRES_DEV esp_lcd driver esp_timer
    REQUIRES_SIM SDL2::SDL2-static
)

if(NOT BUILD_SIMULATOR)
    # -O2 on the fx pass ONLY. Measured 2026-08-21, 8x16 dense:
    #   all -Os  scan 973 instrs, band 573 us, wobble +77,605 cyc
    #   all -O2  scan 1350 instrs (+38.7%), band 766 us (+35%), wobble +49,255
    # -O2 fixes the wobble shift but inflates the hand-tuned scan loop, which
    # dominates — so the flag belongs on this file alone, never the component.
    #
    # Still required after the word-aligned wobble rewrite: at 10x20 the
    # rewrite alone (-Os) leaves the worst band at 519 us against a 512.5 us
    # deadline; with -O2 it is 496 us. The two are complementary.
    set_source_files_properties(render_fx_pass.c
        PROPERTIES COMPILE_OPTIONS "-O2")
endif()
