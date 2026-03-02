include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         src/vtparse.c
    INCLUDE_DIRS include
)
