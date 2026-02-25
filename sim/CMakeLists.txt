cmake_minimum_required(VERSION 3.20)
project(cyberdeck_sim LANGUAGES C)
set(CMAKE_C_STANDARD 11)

add_compile_definitions(BUILD_SIMULATOR=1)

set(ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(STUBS ${CMAKE_CURRENT_SOURCE_DIR}/idf_stubs)

# ---------------------------------------------------------------------------
# CPM — fetch SDL2
# ---------------------------------------------------------------------------
include(${ROOT}/cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       SDL2
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG    release-2.30.11
    GIT_SHALLOW TRUE
    OPTIONS
        "SDL_SHARED OFF"
        "SDL_STATIC ON"
        "SDL_TEST   OFF"
)

# ---------------------------------------------------------------------------
# font — terminus8x16 bitmap font
# ---------------------------------------------------------------------------
add_library(font STATIC
    ${ROOT}/components/font/font_renderer.c
    ${ROOT}/components/font/terminus8x16.c)
target_include_directories(font PUBLIC
    ${ROOT}/components/font/include
    ${STUBS})

# ---------------------------------------------------------------------------
# display — shared render core + SDL2 backend
# ---------------------------------------------------------------------------
add_library(display STATIC
    ${ROOT}/components/display/display_render.c
    ${ROOT}/components/display/display_sdl.c)
target_include_directories(display PUBLIC
    ${ROOT}/components/display/include
    ${STUBS})
target_link_libraries(display PUBLIC font SDL2::SDL2-static)

# ---------------------------------------------------------------------------
# terminal — character grid + UTF-8 decoder
# ---------------------------------------------------------------------------
add_library(terminal STATIC
    ${ROOT}/components/terminal/terminal.c)
target_include_directories(terminal PUBLIC
    ${ROOT}/components/terminal/include
    ${STUBS})
target_link_libraries(terminal PUBLIC display)

# ---------------------------------------------------------------------------
# cyberdeck_sim executable
# ---------------------------------------------------------------------------
add_executable(cyberdeck_sim main.c)
# SDL2main must come first on Windows: it provides WinMain which calls SDL_main
# (SDL.h renames our main() to SDL_main on Windows).
target_link_libraries(cyberdeck_sim PRIVATE SDL2::SDL2main terminal display font)
