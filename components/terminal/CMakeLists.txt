if(BUILD_SIMULATOR)
    add_library(terminal STATIC terminal.c)
    target_include_directories(terminal PUBLIC include)
    target_link_libraries(terminal PUBLIC display idfsim)
else()
    idf_component_register(
        SRCS
            "terminal.c"
        INCLUDE_DIRS
            "include"
        REQUIRES
            display
            font
    )
endif()

# TODO: Add libvterm when ready
# PRIV_REQUIRES libvterm
