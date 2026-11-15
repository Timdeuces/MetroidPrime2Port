#pragma once

#define IMGUI_DISABLE_DEFAULT_FILE_FUNCTIONS 1

#include <SDL2/SDL.h>

typedef SDL_RWops* ImFileHandle;
typedef unsigned long long ImU64;

ImFileHandle ImFileOpen(const char* filename, const char* mode);
bool ImFileClose(ImFileHandle file);
ImU64 ImFileGetSize(ImFileHandle file);
ImU64 ImFileRead(void* data, ImU64 size, ImU64 count, ImFileHandle file);
ImU64 ImFileWrite(const void* data, ImU64 size, ImU64 count, ImFileHandle file);
