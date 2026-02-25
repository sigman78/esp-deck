idf_component_register(
    SRCS
        "terminal.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        display
        font
)

# TODO: Add libvterm when ready
# PRIV_REQUIRES libvterm
