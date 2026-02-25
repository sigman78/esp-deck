cmake_minimum_required(VERSION 3.20)
project(cyberdeck_sim LANGUAGES C)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Mirror the IDF variable so component files can use ${PROJECT_DIR}/cmake/...
set(PROJECT_DIR ${ROOT})

# Preprocessor flag consumed by display.h and display_sdl.c
add_compile_definitions(BUILD_SIMULATOR=1)

include(${ROOT}/cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       SDL2
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG    release-2.30.11
    GIT_SHALLOW TRUE
    OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST OFF"
)

add_subdirectory(${ROOT}/idfsim              idfsim)
add_subdirectory(${ROOT}/components/font     font)
add_subdirectory(${ROOT}/components/display  display)
add_subdirectory(${ROOT}/components/terminal terminal)

add_executable(cyberdeck_sim main.c)
# SDL2main provides WinMain on Windows (SDL.h renames main -> SDL_main)
target_link_libraries(cyberdeck_sim PRIVATE SDL2::SDL2main terminal)
