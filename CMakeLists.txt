cmake_minimum_required(VERSION 3.20)
project(neogeorecomp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(NEOGEORECOMP_BUILD_TESTS "Build test suite" OFF)
option(NEOGEORECOMP_DEBUG "Enable debug tracing and breakpoints" OFF)

# Find SDL2 — try CMake config first, fall back to manual detection
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    # Manual fallback: check common locations
    find_path(SDL2_INCLUDE_DIR SDL.h
        PATH_SUFFIXES SDL2 include/SDL2
        PATHS
            C:/msys64/mingw64
            C:/msys64/ucrt64
            $ENV{SDL2_DIR}
    )
    find_library(SDL2_LIBRARY
        NAMES SDL2
        PATH_SUFFIXES lib
        PATHS
            C:/msys64/mingw64
            C:/msys64/ucrt64
            $ENV{SDL2_DIR}
    )
    find_library(SDL2MAIN_LIBRARY
        NAMES SDL2main
        PATH_SUFFIXES lib
        PATHS
            C:/msys64/mingw64
            C:/msys64/ucrt64
            $ENV{SDL2_DIR}
    )
    if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
        set(SDL2_FOUND TRUE)
        message(STATUS "SDL2 found manually: ${SDL2_LIBRARY}")
    else()
        message(FATAL_ERROR "SDL2 not found. Set SDL2_DIR or install SDL2 development libraries.")
    endif()
endif()

# Collect sources
set(NEOGEORECOMP_SOURCES
    src/neogeorecomp.c
    src/m68k.c
    src/bus.c
    src/func_table.c
    src/video.c
    src/palette.c
    src/io.c
    src/ym2610.c
    src/z80.c
    src/timer.c
    src/platform.c
    src/debug.c
)

# Static library
add_library(neogeorecomp STATIC ${NEOGEORECOMP_SOURCES})

target_include_directories(neogeorecomp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link SDL2 — handle both imported target and manual find
if(TARGET SDL2::SDL2)
    target_link_libraries(neogeorecomp PUBLIC SDL2::SDL2)
else()
    target_include_directories(neogeorecomp PUBLIC ${SDL2_INCLUDE_DIR})
    target_link_libraries(neogeorecomp PUBLIC ${SDL2_LIBRARY})
    if(SDL2MAIN_LIBRARY)
        target_link_libraries(neogeorecomp PUBLIC ${SDL2MAIN_LIBRARY})
    endif()
endif()

if(NEOGEORECOMP_DEBUG)
    target_compile_definitions(neogeorecomp PUBLIC NEOGEORECOMP_DEBUG=1)
endif()

# Install
install(TARGETS neogeorecomp
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/neogeorecomp
    DESTINATION include
)
