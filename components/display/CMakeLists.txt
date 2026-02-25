if(BUILD_SIMULATOR)
    add_library(display STATIC display_render.c display_sdl.c)
    target_include_directories(display PUBLIC include)
    target_link_libraries(display PUBLIC font SDL2::SDL2-static idfsim)
else()
    idf_component_register(
        SRCS
            "display_render.c"
            "lcd_driver.c"
        INCLUDE_DIRS
            "include"
        REQUIRES
            esp_lcd
            driver
            esp_timer
            font
    )
endif()
