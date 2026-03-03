include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         vterm.c
    INCLUDE_DIRS include
    REQUIRES     display libtsm tsm_slim
)

if(DEFINED ENV{IDF_PATH} AND NOT BUILD_SIMULATOR)
target_compile_definitions("${COMPONENT_TARGET}" PUBLIC "CONFIG_VTERM_TSM_SLIM")
else()
target_compile_definitions(vterm PUBLIC "CONFIG_VTERM_TSM_SLIM")
endif()
