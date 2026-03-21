cmake_minimum_required(VERSION 3.20)
project(neogeorecomp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(NEOGEORECOMP_BUILD_TESTS "Build test suite" OFF)
option(NEOGEORECOMP_DEBUG "Enable debug tracing and breakpoints" OFF)

# Find SDL2
find_package(SDL2 REQUIRED)

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

target_link_libraries(neogeorecomp
    PUBLIC
        SDL2::SDL2
)

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
