#include <aurora/main.h>
#undef main

#include <SDL2/SDL_main.h>

int main(int argc, char** argv) { return aurora_main(argc, argv); }
