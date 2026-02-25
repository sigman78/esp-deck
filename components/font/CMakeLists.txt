if(BUILD_SIMULATOR)
    add_library(font STATIC font_renderer.c terminus8x16.c)
    target_include_directories(font PUBLIC include)
    target_link_libraries(font PUBLIC idfsim)
else()
    idf_component_register(
        SRCS
            "font_renderer.c"
            "terminus8x16.c"
        INCLUDE_DIRS
            "include"
    )
endif()
