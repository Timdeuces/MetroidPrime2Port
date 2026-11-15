#include <SDL.h>
#include <stdio.h>

#define GL_COLOR_BUFFER_BIT 0x00004000

typedef void (SDLCALL *GLClearColorProc)(float red, float green, float blue, float alpha);
typedef void (SDLCALL *GLClearProc)(unsigned int mask);
typedef void (SDLCALL *GLViewportProc)(int x, int y, int width, int height);

int main(void) {
  SDL_Window *window = NULL;
  SDL_GLContext context = NULL;
  int result = 1;

  if (SDL_setenv("SDL_VIDEODRIVER", "kmsdrm", 1) != 0) {
    fprintf(stderr, "Could not select the SDL2 KMSDRM video driver: %s\n", SDL_GetError());
    return result;
  }
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    goto cleanup;
  }

  printf("SDL version: %u.%u.%u\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
  printf("Video driver: %s\n", SDL_GetCurrentVideoDriver());

  if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) != 0 ||
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2) != 0 ||
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0) != 0 ||
      SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) != 0) {
    fprintf(stderr, "SDL_GL_SetAttribute failed: %s\n", SDL_GetError());
    goto cleanup;
  }

  window = SDL_CreateWindow("SDL2 KMSDRM probe", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                            640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (window == NULL) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    goto cleanup;
  }

  context = SDL_GL_CreateContext(window);
  if (context == NULL) {
    fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
    goto cleanup;
  }

  GLClearColorProc gl_clear_color = (GLClearColorProc)SDL_GL_GetProcAddress("glClearColor");
  GLClearProc gl_clear = (GLClearProc)SDL_GL_GetProcAddress("glClear");
  GLViewportProc gl_viewport = (GLViewportProc)SDL_GL_GetProcAddress("glViewport");
  if (gl_clear_color == NULL || gl_clear == NULL || gl_viewport == NULL) {
    fprintf(stderr, "Required GLES entry points unavailable: %s\n", SDL_GetError());
    goto cleanup;
  }

  gl_viewport(0, 0, 640, 480);
  for (int frame = 0; frame < 120; ++frame) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        goto completed;
      }
    }

    gl_clear_color((frame & 1) ? 0.05f : 0.05f, (frame & 1) ? 0.15f : 0.05f,
                   (frame & 1) ? 0.75f : 0.15f, 1.0f);
    gl_clear(GL_COLOR_BUFFER_BIT);
    SDL_ClearError();
    SDL_GL_SwapWindow(window);
    const char *error = SDL_GetError();
    if (error != NULL && error[0] != '\0') {
      fprintf(stderr, "SDL_GL_SwapWindow failed on frame %d: %s\n", frame, error);
      goto cleanup;
    }
    SDL_Delay(16);
  }

completed:
  printf("SDL2 KMSDRM GLES probe passed: presented frames without SDL error\n");
  result = 0;

cleanup:
  if (context != NULL) {
    SDL_GL_DeleteContext(context);
  }
  if (window != NULL) {
    SDL_DestroyWindow(window);
  }
  SDL_Quit();
  return result;
}
