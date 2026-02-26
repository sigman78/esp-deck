set(LIBTSM_SRCS
    vendor/tsm-render.c
    vendor/tsm-screen.c
    vendor/tsm-selection.c
    vendor/tsm-unicode.c
    vendor/tsm-vte-charsets.c
    vendor/tsm-vte.c
    vendor/shl-htable.c
    vendor/wcwidth.c
)

if(BUILD_SIMULATOR)
    add_library(libtsm STATIC ${LIBTSM_SRCS})
    target_include_directories(libtsm
        PUBLIC  include
        PRIVATE vendor)
    target_compile_definitions(libtsm PRIVATE BUILD_ENABLE_DEBUG=0)
    target_link_libraries(libtsm PUBLIC idfsim)
    target_compile_options(libtsm PRIVATE
        -Wno-sign-compare
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -Wno-format)
else()
    idf_component_register(
        SRCS             ${LIBTSM_SRCS}
        INCLUDE_DIRS     include
        PRIV_INCLUDE_DIRS vendor)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_ENABLE_DEBUG=0)
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-sign-compare
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -Wno-format
        -include ${CMAKE_CURRENT_SOURCE_DIR}/spiram_alloc.h)
endif()
