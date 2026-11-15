include_guard(GLOBAL)

if (NOT AURORA_SDL2_PROVIDER STREQUAL "system")
  message(FATAL_ERROR "Unsupported AURORA_SDL2_PROVIDER: ${AURORA_SDL2_PROVIDER}")
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET GLOBAL sdl2)
set(AURORA_SDL_TARGET PkgConfig::SDL2 CACHE INTERNAL "SDL2 target used by Aurora")
message(STATUS "aurora: Using system SDL2 (${SDL2_VERSION})")
