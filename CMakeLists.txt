cmake_minimum_required(VERSION 3.20)
project(neogeorecomp VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(NEOGEORECOMP_BUILD_TESTS "Build the conformance harness and unit tests" ON)

set(NG_EXT ${CMAKE_CURRENT_SOURCE_DIR}/ext)
if(NOT EXISTS ${NG_EXT}/musashi/m68k_in.c OR NOT EXISTS ${NG_EXT}/z80/z80.c)
    message(FATAL_ERROR "Submodules missing: run  git submodule update --init")
endif()

# ---- Musashi (MIT): 68000 interpreter, the fallback and the oracle ----
# m68kmake generates the opcode handlers from m68k_in.c at build time.
add_executable(m68kmake ${NG_EXT}/musashi/m68kmake.c)
if(MSVC)
    target_compile_definitions(m68kmake PRIVATE _CRT_SECURE_NO_WARNINGS)
    # MSVC 19.4x at /O2 miscompiles m68kmake's mask computation (bit 14 of
    # every opcode mask is lost, e.g. 0xb000 for 0xf000), which crashes
    # m68k_init. It is a one-shot generator, so build it unoptimized.
    target_compile_options(m68kmake PRIVATE /Od)
endif()
set(MUSASHI_GEN ${CMAKE_CURRENT_BINARY_DIR}/musashi_gen)
file(MAKE_DIRECTORY ${MUSASHI_GEN})
add_custom_command(
    OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
    COMMAND m68kmake ${MUSASHI_GEN} ${NG_EXT}/musashi/m68k_in.c
    DEPENDS m68kmake ${NG_EXT}/musashi/m68k_in.c
    COMMENT "Generating Musashi opcode tables")

add_library(musashi STATIC
    ${NG_EXT}/musashi/m68kcpu.c
    ${NG_EXT}/musashi/m68kdasm.c
    ${NG_EXT}/musashi/softfloat/softfloat.c
    ${MUSASHI_GEN}/m68kops.c
    src/musashi_glue.c)
target_include_directories(musashi PUBLIC ${NG_EXT}/musashi ${MUSASHI_GEN} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(musashi PUBLIC MUSASHI_CNF="ng_musashi_conf.h")
if(MSVC)
    target_compile_definitions(musashi PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(musashi PRIVATE /w)
else()
    target_compile_options(musashi PRIVATE -w)
endif()

# ---- Z80 (MIT) ----
add_library(z80core STATIC ${NG_EXT}/z80/z80.c)
target_include_directories(z80core PUBLIC ${NG_EXT}/z80)
if(MSVC)
    target_compile_options(z80core PRIVATE /w)
endif()

# ---- runtime ----
find_package(SDL2 CONFIG REQUIRED)

add_library(neogeorecomp STATIC
    src/neogeo.c
    src/exec.c
    src/bus.c
    src/lspc.c
    src/video.c
    src/io.c
    src/audio.c
    src/platform.c)
target_include_directories(neogeorecomp
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(neogeorecomp PUBLIC musashi z80core SDL2::SDL2)
if(MSVC)
    target_compile_definitions(neogeorecomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(neogeorecomp PRIVATE /W3)
else()
    target_compile_options(neogeorecomp PRIVATE -Wall -Wextra)
endif()

if(NEOGEORECOMP_BUILD_TESTS)
    enable_testing()
endif()
