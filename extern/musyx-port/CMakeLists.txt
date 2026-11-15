cmake_minimum_required(VERSION 3.13)
project(MusyX)

set(MUSY_TARGET MUSY_TARGET_PC)

set(CMAKE_C_STANDARD 23)
add_library(musyx STATIC
        src/musyx/runtime/seq.c
        src/musyx/runtime/synth.c
        src/musyx/runtime/seq_api.c
        src/musyx/runtime/snd_synthapi.c
        src/musyx/runtime/stream.c
        src/musyx/runtime/synthdata.c
        src/musyx/runtime/synthmacros.c
        src/musyx/runtime/synthvoice.c
        src/musyx/runtime/synth_ac.c
        src/musyx/runtime/synth_adsr.c
        src/musyx/runtime/synth_vsamples.c
        src/musyx/runtime/synth_dbtab.c
        src/musyx/runtime/s_data.c
        src/musyx/runtime/hw_dspctrl.c
        src/musyx/runtime/hw_volconv.c
        src/musyx/runtime/snd3d.c
        src/musyx/runtime/snd_init.c
        src/musyx/runtime/snd_math.c
        src/musyx/runtime/snd_midictrl.c
        src/musyx/runtime/snd_service.c
        src/musyx/runtime/hardware.c
        src/musyx/runtime/hw_aramdma.c
        src/musyx/runtime/dsp_import.c
        src/musyx/runtime/hw_dolphin.c
        src/musyx/runtime/hw_pc.c
        src/musyx/runtime/hw_memory.c
        src/musyx/runtime/hw_lib_dolphin.c
        src/musyx/runtime/CheapReverb/creverb_fx.c
        src/musyx/runtime/CheapReverb/creverb.c
        src/musyx/runtime/StdReverb/reverb_fx.c
        src/musyx/runtime/StdReverb/reverb.c
        src/musyx/runtime/Delay/delay_fx.c
        src/musyx/runtime/Chorus/chorus_fx.c
)

target_include_directories(musyx PUBLIC include)
if (CMAKE_SYSTEM_NAME STREQUAL Linux)
target_compile_options(musyx PRIVATE -Wno-stringop-overflow -Wno-pointer-to-int-cast -gdwarf-4)
else ()
target_compile_options(musyx PRIVATE)
endif ()
# MusyX 2.0.x revision the game was built against (Prime 1: 2.0.0, Echoes: 2.0.3).
set(MUSYX_VERSION_PATCH 0 CACHE STRING "MusyX 2.0.x patch version to build")
target_compile_definitions(musyx PUBLIC
        -DMUSY_TARGET=${MUSY_TARGET}
        -DMUSY_VERSION_MAJOR=2 -DMUSY_VERSION_MINOR=0 -DMUSY_VERSION_PATCH=${MUSYX_VERSION_PATCH}
        $<$<CONFIG:Release>:NDEBUG>
        $<$<CONFIG:MinSizeRel>:NDEBUG>)
# hardware.c uses M_PI, which the MSVC CRT's math.h only defines on request.
target_compile_definitions(musyx PRIVATE $<$<BOOL:${WIN32}>:_USE_MATH_DEFINES>)
set_target_properties(musyx PROPERTIES DEBUG_POSTFIX D)
set_target_properties(musyx PROPERTIES RELWITHDEBINFO_POSTFIX RD)
set_target_properties(musyx PROPERTIES MINSIZEREL_POSTFIX MIN)

# SDL2 is provided by Aurora when MusyX is built as part of the port.
if (DEFINED AURORA_SDL_TARGET AND TARGET ${AURORA_SDL_TARGET})
  set(MUSYX_SDL_TARGET ${AURORA_SDL_TARGET})
elseif (TARGET SDL2::SDL2)
  set(MUSYX_SDL_TARGET SDL2::SDL2)
else ()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2)
  set(MUSYX_SDL_TARGET PkgConfig::SDL2)
endif ()
target_link_libraries(musyx PUBLIC ${MUSYX_SDL_TARGET})

option(MUSYX_BUILD_PLAYER "Build the standalone MusyX sample player" OFF)
if(MUSYX_BUILD_PLAYER)
  add_executable(musyx_player test/main.c)
  target_link_libraries(musyx_player PRIVATE musyx)
  if(NOT MSVC)
    target_link_libraries(musyx_player PRIVATE m)
  endif()
endif()
