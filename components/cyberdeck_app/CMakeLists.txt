include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         cyberdeck_app.c
    INCLUDE_DIRS include
    REQUIRES     storage ssh wifi
)
