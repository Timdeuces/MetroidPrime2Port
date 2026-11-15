#include "window.hpp"

#ifdef AURORA_ENABLE_GX
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#endif
#include "input.hpp"
#include "internal.hpp"
#include "screenshot.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_hints.h>
#include <SDL2/SDL_joystick.h>
#include <SDL2/SDL_pixels.h>
#include <SDL2/SDL_syswm.h>
#include <tracy/Tracy.hpp>

#if defined(SDL_PLATFORM_ANDROID)
#include <jni.h>
extern "C" void Android_LockActivityMutex(void);
extern "C" void Android_UnlockActivityMutex(void);
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "rmlui.hpp"
#include "time_internal.hpp"
#include "dolphin/vi/vi_internal.hpp"

namespace aurora::window {
namespace {
constexpr Module Log{"aurora::window"};

SDL_Window* g_window;
SDL_Renderer* g_renderer;
SDL_GLContext g_glContext;
void* g_kmsdrmEglSurface;
void* g_kmsdrmEglDisplay;
float g_frameBufferScale = 0.f;
bool g_frameBufferAspectFit = false;
AuroraWindowSize g_windowSize;
std::vector<AuroraEvent> g_events;
std::deque<std::string> g_eventStrings;
std::atomic_bool g_backgrounded = false;
#if defined(SDL_PLATFORM_ANDROID)
std::atomic_bool g_surfaceReady = false;
#else
std::atomic_bool g_surfaceReady = true;
#endif
// Set when the platform surface is destroyed. The swapchain still refers to the
// old native window and must be rebuilt before it can be configured again.
std::atomic_bool g_surfaceInvalidated = false;
// Set while Android's surfaceChanged runs. The native window stays the same, so the
// surface is kept: presenting pauses, and a refresh afterwards picks up the new size.
std::atomic_bool g_surfaceChanging = false;
std::mutex g_surfaceHeldMutex;
std::condition_variable g_surfaceHeldCv;
bool g_surfaceHeld = false;
bool g_lastPaused = false;
uint64_t g_joystickUpdateNs = 0;
bool g_gotFocus = false;

void retain_event_strings(SDL_Event& event) {
  switch (event.type) {
  case SDL_DROPFILE:
  case SDL_DROPTEXT:
    if (event.drop.file != nullptr) {
      g_eventStrings.emplace_back(event.drop.file);
      SDL_free(event.drop.file);
      event.drop.file = const_cast<char*>(g_eventStrings.back().c_str());
    }
    break;
  default:
    return;
  }
}

bool operator==(const AuroraWindowSize& lhs, const AuroraWindowSize& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.fb_width == rhs.fb_width &&
         lhs.fb_height == rhs.fb_height && lhs.native_fb_height == rhs.native_fb_height &&
         lhs.native_fb_width == rhs.native_fb_width && lhs.scale == rhs.scale;
}

Vec2<int> scale_frame_buffer_to_aspect(int w, int h, float scale, float aspect) {
  if (w <= 0 || h <= 0 || scale <= 0.f || aspect <= 0.f) {
    return {std::max(w, 1), std::max(h, 1)};
  }
  const int baseW = std::max(1, static_cast<int>(std::lround(static_cast<float>(w) * scale)));
  const int baseH = std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * scale)));
  if (aspect >= static_cast<float>(w) / static_cast<float>(h)) {
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(baseH) * aspect))), baseH};
  }
  return {baseW, std::max(1, static_cast<int>(std::lround(static_cast<float>(baseW) / aspect)))};
}

Vec2<int> fit_frame_buffer_to_aspect(int width, int height, float aspect) {
  if (width <= 0 || height <= 0 || aspect <= 0.f) {
    return {std::max(width, 1), std::max(height, 1)};
  }
  if (static_cast<float>(width) / static_cast<float>(height) > aspect) {
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * aspect))), height};
  }
  return {width, std::max(1, static_cast<int>(std::lround(static_cast<float>(width) / aspect)))};
}

void resize_swapchain() noexcept {
  const auto size = get_window_size();
  if (size == g_windowSize) {
    return;
  }
  if (size.scale != g_windowSize.scale) {
    if (g_windowSize.scale > 0.f) {
      Log.info("Display scale changed to {}", size.scale);
    }
  }
  g_windowSize = size;
  if (g_renderer != nullptr) {
    SDL_RenderSetScale(g_renderer, size.scale, size.scale);
  }
#ifdef AURORA_ENABLE_GX
  if (is_surface_changing()) {
    return;
  }
  const bool invalidated = consume_surface_invalidated();
  if (!is_presentable() || (invalidated && webgpu::surface_window_changed())) {
    // The surface was destroyed since the swapchain was created, so configuring
    // it would target a native window that no longer exists (and can take the
    // whole device down). Drop it; the next frame rebuilds it against the new
    // surface. This also matters when a blocking dialog outlives the surface.
    // Android can also rebuild the surface onto the same window, and asking for
    // a second surface for one window fails, so only rebuild when it moved.
    webgpu::release_surface();
    return;
  }
  webgpu::resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height);
#endif
}

void set_window_icon() noexcept {
  if (g_config.iconRGBA8 == nullptr) {
    return;
  }
  auto* iconSurface = SDL_CreateRGBSurfaceFrom(g_config.iconRGBA8, static_cast<int>(g_config.iconWidth),
                                                static_cast<int>(g_config.iconHeight), 32,
                                                static_cast<int>(4 * g_config.iconWidth), 0x000000ff,
                                                0x0000ff00, 0x00ff0000, 0xff000000);
  AURORA_ASSERT(iconSurface != nullptr, "Failed to create icon surface: {}", SDL_GetError());
  SDL_SetWindowIcon(g_window, iconSurface);
  SDL_FreeSurface(iconSurface);
}

bool targets_primary_window(const SDL_Event* event) noexcept {
  Uint32 eventWindowID = 0;
  switch (event->type) {
  case SDL_WINDOWEVENT:
    eventWindowID = event->window.windowID;
    break;
  case SDL_KEYDOWN:
  case SDL_KEYUP:
    eventWindowID = event->key.windowID;
    break;
  case SDL_MOUSEMOTION:
    eventWindowID = event->motion.windowID;
    break;
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP:
    eventWindowID = event->button.windowID;
    break;
  case SDL_MOUSEWHEEL:
    eventWindowID = event->wheel.windowID;
    break;
  default:
    break;
  }
  const SDL_Window* eventWindow = eventWindowID == 0 ? nullptr : SDL_GetWindowFromID(eventWindowID);
  return eventWindow == nullptr || eventWindow == g_window;
}

int SDLCALL lifecycle_event_watch(void*, SDL_Event* event) {
  if (targets_primary_window(event)) {
    switch (event->type) {
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_APPLE)
    case SDL_WINDOWEVENT_MINIMIZED:
      time::internal::set_pause_reason(time::internal::PauseReason::Background, true);
      g_backgrounded.store(true, std::memory_order_relaxed);
      break;
    case SDL_WINDOWEVENT_RESTORED:
      g_backgrounded.store(false, std::memory_order_relaxed);
      time::internal::set_pause_reason(time::internal::PauseReason::Background, false);
      break;
#endif
    default:
      break;
    }
  }
  return true;
}

void sync_paused() {
  const bool paused = is_paused();
  if (g_lastPaused == paused) {
    return;
  }
  g_lastPaused = paused;
  time::internal::set_pause_reason(time::internal::PauseReason::Window, paused);
  g_events.push_back(AuroraEvent{
      .type = paused ? AURORA_PAUSED : AURORA_UNPAUSED,
  });
}

void process_event(SDL_Event& event) {
  const bool primaryWindow = targets_primary_window(&event);
  if (primaryWindow) {
#ifdef AURORA_ENABLE_GX
    imgui::process_event(event);
#endif
#ifdef AURORA_ENABLE_RMLUI
    rmlui::handle_event(event);
#endif
  }

  switch (event.type) {
  case SDL_WINDOWEVENT: {
    if (!primaryWindow) {
      break;
    }
    if (event.window.event == SDL_WINDOWEVENT_MOVED) {
      g_events.push_back(AuroraEvent{
          .type = AURORA_WINDOW_MOVED,
          .windowPos = {.x = event.window.data1, .y = event.window.data2},
      });
    } else if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
               event.window.event == SDL_WINDOWEVENT_RESIZED) {
      resize_swapchain();
      g_events.push_back(AuroraEvent{
          .type = AURORA_WINDOW_RESIZED,
          .windowSize = get_window_size(),
      });
    } else if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
      g_events.push_back(AuroraEvent{
        .type = AURORA_EXIT,
      });
    }
    break;
  }
  case SDL_CONTROLLERDEVICEADDED: {
    auto instance = input::add_controller(event.cdevice.which);
    g_events.push_back(AuroraEvent{
        .type = AURORA_CONTROLLER_ADDED,
        .controller = instance,
    });
    break;
  }
  case SDL_CONTROLLERDEVICEREMOVED: {
    input::remove_controller(event.cdevice.which);
    g_events.push_back(AuroraEvent{
        .type = AURORA_CONTROLLER_REMOVED,
        .controller = event.cdevice.which,
    });
    break;
  }
  case SDL_MOUSEWHEEL:
    if (primaryWindow) {
      input::set_mouse_scroll(event.wheel.x, event.wheel.y);
    }
    break;
  case SDL_QUIT:
    g_events.push_back(AuroraEvent{
        .type = AURORA_EXIT,
    });
    break;
  default:
    if (event.type == g_sdlCustomEventsStart + CustomEvent::FutureResize) {
      // Future resize event
      resize_swapchain();
      g_events.push_back(AuroraEvent{
          .type = AURORA_WINDOW_RESIZED,
          .windowSize = get_window_size(),
      });
    } else if (event.type == g_sdlCustomEventsStart + CustomEvent::RefreshSurface) {
      // Refresh surface (vsync changed)
#ifdef AURORA_ENABLE_GX
      webgpu::refresh_surface(false);
#endif
    }
    break;
  }

  if (primaryWindow) {
    sync_paused();
  }
  retain_event_strings(event);
  g_events.push_back(AuroraEvent{
      .type = AURORA_SDL_EVENT,
      .sdl = event,
  });
}
} // namespace

const AuroraEvent* poll_events() {
  ZoneScoped;
  g_events.clear();
  g_eventStrings.clear();

  SDL_Event event;
  // Clear out the previous scroll values to prevent ghost input
  input::set_mouse_scroll(0, 0);
  // What SDL_PumpEvents would do first (SDL_HINT_AUTO_UPDATE_JOYSTICKS is off), timed on its
  // own: on Windows a device change re-enumerates DirectInput and HID devices here, on this
  // thread, and a slow driver makes that a freeze of seconds (issue #278).
  {
    ZoneScopedN("SDL_UpdateJoysticks");
    const uint64_t start = SDL_GetTicks64() * 1000000ULL;
    SDL_JoystickUpdate();
    g_joystickUpdateNs = SDL_GetTicks64() * 1000000ULL - start;
  }
  if (is_paused()) {
    // The application may service main-thread audio and I/O completions even
    // while it cannot render. Do not block its pump until the window returns.
    ZoneScopedN("SDL_WaitEventTimeout (paused)");
    if (SDL_WaitEventTimeout(&event, 5)) {
      process_event(event);
    }
  }
  while (true) {
    bool hasEvent = false;
    {
      ZoneScopedN("SDL_PollEvent");
      hasEvent = SDL_PollEvent(&event);
    }
    if (hasEvent) {
      process_event(event);
    } else {
      break;
    }
  }
  g_events.push_back(AuroraEvent{
      .type = AURORA_NONE,
  });
  return g_events.data();
}

bool create_window(AuroraBackend backend) {
  Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;
#if TARGET_OS_IOS || TARGET_OS_TV
  flags |= SDL_WINDOW_FULLSCREEN;
#else
  flags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
  if (g_config.startFullscreen) {
    flags |= SDL_WINDOW_FULLSCREEN;
  }
#endif
#if defined(__linux__)
  const char* videoDriver = SDL_GetCurrentVideoDriver();
  if (videoDriver != nullptr && SDL_strcasecmp(videoDriver, "kmsdrm") == 0) {
    flags |= SDL_WINDOW_FULLSCREEN;
    if (backend == BACKEND_VULKAN) {
      flags |= SDL_WINDOW_VULKAN;
    } else if (backend == BACKEND_OPENGL || backend == BACKEND_OPENGLES) {
      flags |= SDL_WINDOW_OPENGL;
      if (backend == BACKEND_OPENGLES) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
      } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
      }
    }
  }
#endif
  auto width = static_cast<Sint32>(g_config.windowWidth);
  auto height = static_cast<Sint32>(g_config.windowHeight);
  if (width == 0 || height == 0) {
    width = 1280;
    height = 960;
  }
  if (width < 640) {
    width = 640;
  }
  if (height < 480) {
    height = 480;
  }
  // Shrink a default that doesn't fit the desktop, keeping its aspect ratio.
  SDL_Rect usable{};
  if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
    const float fit = std::min(static_cast<float>(usable.w) / static_cast<float>(width),
                               static_cast<float>(usable.h) / static_cast<float>(height));
    if (fit < 1.f) {
      width = std::max(640, static_cast<Sint32>(static_cast<float>(width) * fit));
      height = std::max(480, static_cast<Sint32>(static_cast<float>(height) * fit));
    }
  }

  Sint32 posX = g_config.windowPosX;
  Sint32 posY = g_config.windowPosY;
  if (posX < 0 || posY < 0) {
    posX = SDL_WINDOWPOS_UNDEFINED;
    posY = SDL_WINDOWPOS_UNDEFINED;
  }

    g_window = SDL_CreateWindow(g_config.appName, posX, posY, width, height, flags);
  if (g_window == nullptr) {
    Log.error("Failed to create window: {}", SDL_GetError());
    return false;
  }
#if defined(__linux__)
  if ((flags & SDL_WINDOW_OPENGL) != 0) {
    g_glContext = SDL_GL_CreateContext(g_window);
    if (g_glContext == nullptr) {
      Log.error("Failed to create KMSDRM GL context: {}", SDL_GetError());
      SDL_DestroyWindow(g_window);
      g_window = nullptr;
      return false;
    }
    if (videoDriver != nullptr && SDL_strcasecmp(videoDriver, "kmsdrm") == 0) {
      using EGLGetCurrentSurfaceProc = void* (SDLCALL *)(unsigned int);
      using EGLGetCurrentDisplayProc = void* (SDLCALL *)(void);
      const auto getCurrentSurface = reinterpret_cast<EGLGetCurrentSurfaceProc>(
          SDL_GL_GetProcAddress("eglGetCurrentSurface"));
      const auto getCurrentDisplay = reinterpret_cast<EGLGetCurrentDisplayProc>(
          SDL_GL_GetProcAddress("eglGetCurrentDisplay"));
      if (getCurrentSurface == nullptr) {
        Log.error("Failed to load eglGetCurrentSurface from SDL2: {}", SDL_GetError());
        SDL_GL_DeleteContext(g_glContext);
        g_glContext = nullptr;
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return false;
      }
      if (getCurrentDisplay == nullptr) {
        Log.error("Failed to load eglGetCurrentDisplay from SDL2: {}", SDL_GetError());
        SDL_GL_DeleteContext(g_glContext);
        g_glContext = nullptr;
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return false;
      }
      g_kmsdrmEglSurface = getCurrentSurface(0x3059);
      g_kmsdrmEglDisplay = getCurrentDisplay();
      if (g_kmsdrmEglSurface == nullptr || g_kmsdrmEglDisplay == nullptr) {
        Log.error("SDL2 KMSDRM EGL context has no current display or draw surface");
        SDL_GL_DeleteContext(g_glContext);
        g_glContext = nullptr;
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return false;
      }
      Log.info("Captured SDL2 KMSDRM EGL display and draw surface for Dawn");
      if (SDL_GL_MakeCurrent(nullptr, nullptr) != 0) {
        Log.error("Failed to release SDL's KMSDRM GL context after surface capture: {}", SDL_GetError());
        SDL_GL_DeleteContext(g_glContext);
        g_glContext = nullptr;
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        g_kmsdrmEglSurface = nullptr;
        g_kmsdrmEglDisplay = nullptr;
        return false;
      }
    }
  }
#endif
  SDL_SetWindowMinimumSize(g_window, 640, 480);
  set_window_icon();
  return true;
}

bool create_renderer() {
  if (g_window == nullptr) {
    return false;
  }
  const Uint32 flags = g_config.vsync ? SDL_RENDERER_PRESENTVSYNC : 0;
  g_renderer = SDL_CreateRenderer(g_window, -1, flags);
  if (g_renderer == nullptr) {
    Log.error("Failed to create renderer: {}", SDL_GetError());
    return false;
  }
  return true;
}

void destroy_window() {
  if (g_renderer != nullptr) {
    SDL_DestroyRenderer(g_renderer);
    g_renderer = nullptr;
  }
  if (g_window != nullptr) {
    if (g_glContext != nullptr) {
      SDL_GL_DeleteContext(g_glContext);
      g_glContext = nullptr;
    }
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
  }
  g_kmsdrmEglSurface = nullptr;
  g_kmsdrmEglDisplay = nullptr;
}

void show_window() {
  if (g_window != nullptr) {
    SDL_ShowWindow(g_window);
  }
}

bool initialize() {
  /* We don't want to initialize anything input related here, otherwise the add events will get lost to the void */
  TRY(SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight"), "Error setting {}: {}", SDL_HINT_ORIENTATIONS,
      SDL_GetError());
  if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_VIDEO) != 0) {
    Log.error("Error initializing SDL: {}", SDL_GetError());
    return false;
  }
  time::internal::set_pause_reason(time::internal::PauseReason::Surface,
                                   !g_surfaceReady.load(std::memory_order_acquire));

#if !defined(_WIN32) && !defined(__APPLE__)
  TRY(SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0"), "Error setting {}: {}",
      SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, SDL_GetError());
#endif
    TRY(SDL_SetHint(SDL_HINT_JOYSTICK_GAMECUBE_RUMBLE_BRAKE, "1"), "Error setting {}: {}",
      SDL_HINT_JOYSTICK_GAMECUBE_RUMBLE_BRAKE, SDL_GetError());
  // poll_events updates the joysticks itself, timed.
  TRY(SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0"), "Error setting {}: {}", SDL_HINT_AUTO_UPDATE_JOYSTICKS,
      SDL_GetError());

  SDL_DisableScreenSaver();
  if (g_config.allowJoystickBackgroundEvents) {
    TRY(SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"), "Error setting {}: {}",
        SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, SDL_GetError());
  }

  return true;
}

bool initialize_event_watch() {
  SDL_AddEventWatch(lifecycle_event_watch, nullptr);
  return true;
}

void shutdown() {
  SDL_DelEventWatch(lifecycle_event_watch, nullptr);
  destroy_window();
  SDL_EnableScreenSaver();
  SDL_Quit();
}

namespace {
// The frame buffer size a render scale gives for a window of native_fb_w x native_fb_h pixels.
Vec2<int> frame_buffer_size_for_scale(int native_fb_w, int native_fb_h, float frameBufferScale) {
  int fb_w = native_fb_w;
  int fb_h = native_fb_h;
  if (frameBufferScale > 0.f) {
    const auto [baseW, baseH] = vi::configured_fb_size();
    const auto [scaledW, scaledH] =
        scale_frame_buffer_to_aspect(static_cast<int>(baseW), static_cast<int>(baseH), frameBufferScale,
                                     static_cast<float>(fb_w) / static_cast<float>(fb_h));
    fb_w = scaledW;
    fb_h = scaledH;
  }
  if (g_frameBufferAspectFit) {
    const auto [baseW, baseH] = vi::configured_fb_size();
    if (baseW > 0 && baseH > 0) {
      const auto [fitW, fitH] =
          fit_frame_buffer_to_aspect(fb_w, fb_h, static_cast<float>(baseW) / static_cast<float>(baseH));
      fb_w = fitW;
      fb_h = fitH;
    }
  }
  // Port: a high scale in a wide window can pass the device's texture size limit, which the EFB
  // targets fail to allocate at: shrink it to fit, keeping the aspect.
  const int maxDim = static_cast<int>(std::min<uint32_t>(max_texture_dimension(), INT32_MAX));
  if (maxDim > 0 && (fb_w > maxDim || fb_h > maxDim)) {
    const float shrink = static_cast<float>(maxDim) / static_cast<float>(std::max(fb_w, fb_h));
    fb_w = std::clamp(static_cast<int>(static_cast<float>(fb_w) * shrink), 1, maxDim);
    fb_h = std::clamp(static_cast<int>(static_cast<float>(fb_h) * shrink), 1, maxDim);
  }
  return {fb_w, fb_h};
}
} // namespace

bool get_frame_buffer_size_for_scale(float scale, uint32_t* width, uint32_t* height) {
  int native_fb_w = 0;
  int native_fb_h = 0;
  if (g_window == nullptr) {
    return false;
  }
  if (g_glContext != nullptr) {
    SDL_GL_GetDrawableSize(g_window, &native_fb_w, &native_fb_h);
  } else {
    SDL_GetWindowSize(g_window, &native_fb_w, &native_fb_h);
  }
  if (native_fb_w <= 0 || native_fb_h <= 0) {
    return false;
  }
  const auto [fb_w, fb_h] = frame_buffer_size_for_scale(native_fb_w, native_fb_h, scale);
  *width = static_cast<uint32_t>(fb_w);
  *height = static_cast<uint32_t>(fb_h);
  return true;
}

AuroraWindowSize get_window_size() {
  int width = 0;
  int height = 0;
  int native_fb_w = 0;
  int native_fb_h = 0;
  SDL_GetWindowSize(g_window, &width, &height);
  if (g_glContext != nullptr) {
    SDL_GL_GetDrawableSize(g_window, &native_fb_w, &native_fb_h);
  } else {
    native_fb_w = width;
    native_fb_h = height;
  }

  const auto [fb_w, fb_h] = frame_buffer_size_for_scale(native_fb_w, native_fb_h, g_frameBufferScale);

  float dpi = 96.f;
  float diagonal = 0.f;
  float horizontal = 0.f;
  float vertical = 0.f;
  const int displayIndex = SDL_GetWindowDisplayIndex(g_window);
  const float scale = displayIndex >= 0 && SDL_GetDisplayDPI(displayIndex, &diagonal, &horizontal, &vertical) == 0
                          ? diagonal / 96.f
                          : dpi / 96.f;
  return {
      .width = static_cast<uint32_t>(width),
      .height = static_cast<uint32_t>(height),
      .fb_width = static_cast<uint32_t>(fb_w),
      .fb_height = static_cast<uint32_t>(fb_h),
      .native_fb_width = static_cast<uint32_t>(native_fb_w),
      .native_fb_height = static_cast<uint32_t>(native_fb_h),
      .scale = scale,
  };
}

SDL_Window* get_sdl_window() { return g_window; }
void* get_kmsdrm_egl_surface(SDL_Window* window) noexcept {
  return window == g_window ? g_kmsdrmEglSurface : nullptr;
}
void* get_kmsdrm_egl_display() noexcept { return g_kmsdrmEglDisplay; }
void* get_kmsdrm_egl_proc(const char* proc) noexcept {
  return SDL_GL_GetProcAddress(proc);
}
bool make_kmsdrm_gl_context_current(bool current) noexcept {
  if (g_window == nullptr || g_glContext == nullptr) {
    return false;
  }
  return SDL_GL_MakeCurrent(current ? g_window : nullptr, current ? g_glContext : nullptr) == 0;
}

const void* get_native_window_handle() noexcept {
  if (g_window == nullptr) {
    return nullptr;
  }
#if defined(SDL_PLATFORM_ANDROID)
  // Changes when Android tears the SurfaceView's surface down and rebuilds it.
  return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window),
                                SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#else
  // The window itself is the identity everywhere else.
  return g_window;
#endif
}

SDL_Renderer* get_sdl_renderer() { return g_renderer; }

uint64_t joystick_update_ns() noexcept { return g_joystickUpdateNs; }

bool is_paused() noexcept {
  if (!is_presentable()) {
    return true;
  }
  const auto flags = SDL_GetWindowFlags(g_window);
  if ((flags & SDL_WINDOW_HIDDEN) != 0u) {
    return true;
  }
  // Wait until the window has received focus before respecting pauseOnFocusLost
  if (!g_gotFocus) {
    g_gotFocus = (flags & SDL_WINDOW_INPUT_FOCUS) != 0u;
    return false;
  }
  return g_config.pauseOnFocusLost && ((flags & SDL_WINDOW_INPUT_FOCUS) == 0u || (flags & SDL_WINDOW_MINIMIZED) != 0u);
}

bool is_presentable() noexcept {
  return g_window != nullptr && !g_backgrounded.load(std::memory_order_acquire) &&
         g_surfaceReady.load(std::memory_order_acquire) && !g_surfaceChanging.load(std::memory_order_acquire);
}

bool is_surface_changing() noexcept { return g_surfaceChanging.load(std::memory_order_acquire); }

void set_surface_changing(bool changing) noexcept {
  g_surfaceChanging.store(changing, std::memory_order_release);
  if (!changing && g_window != nullptr) {
    push_custom_event(CustomEvent::RefreshSurface);
  }
}

bool is_backgrounded() noexcept { return g_backgrounded.load(std::memory_order_acquire); }

bool is_surface_ready() noexcept { return g_surfaceReady.load(std::memory_order_acquire); }

void set_surface_ready(bool ready) noexcept {
  const bool wasReady = g_surfaceReady.exchange(ready, std::memory_order_acq_rel);
  time::internal::set_pause_reason(time::internal::PauseReason::Surface, !ready);
  if (wasReady && !ready) {
    g_surfaceInvalidated.store(true, std::memory_order_release);
  }
}

bool consume_surface_invalidated() noexcept {
  return g_surfaceInvalidated.exchange(false, std::memory_order_acq_rel);
}

void set_surface_held(bool held) noexcept {
  {
    std::lock_guard lock{g_surfaceHeldMutex};
    g_surfaceHeld = held;
  }
  if (!held) {
    g_surfaceHeldCv.notify_all();
  }
}

bool wait_surface_released(int timeoutMs) noexcept {
  std::unique_lock lock{g_surfaceHeldMutex};
  return g_surfaceHeldCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return !g_surfaceHeld; });
}

SurfaceLock::SurfaceLock() noexcept {
#if defined(SDL_PLATFORM_ANDROID)
  Android_LockActivityMutex();
#endif
}

SurfaceLock::~SurfaceLock() {
#if defined(SDL_PLATFORM_ANDROID)
  Android_UnlockActivityMutex();
#endif
}

bool push_custom_event(CustomEvent eventType) {
  SDL_Event event{.type = g_sdlCustomEventsStart + eventType};
  return SDL_PushEvent(&event);
}

void set_title(const char* title) {
  SDL_SetWindowTitle(g_window, title);
}

void set_fullscreen(bool fullscreen) {
  if (SDL_SetWindowFullscreen(g_window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
    Log.warn("Failed to set window fullscreen: {}", SDL_GetError());
  }
}

bool get_fullscreen() { return (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0u; }

void set_window_size(uint32_t width, uint32_t height) {
  SDL_RestoreWindow(g_window);
  SDL_SetWindowSize(g_window, static_cast<int>(width), static_cast<int>(height));
}

void set_window_position(uint32_t x, uint32_t y) {
  SDL_SetWindowPosition(g_window, static_cast<int>(x), static_cast<int>(y));
}

void center_window() {
  SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

static void push_future_resize_event() {
  TRY_WARN(push_custom_event(CustomEvent::FutureResize), "Failed to push SDL event for future resize: {}",
           SDL_GetError());
}

void request_frame_buffer_resize() {
  if (g_window != nullptr) {
    // Defer so that we don't try to reconfigure the surface in the middle of game logic.
    push_future_resize_event();
  }
}

void set_frame_buffer_scale(float scale) {
  if (scale < 0.f) {
    scale = 0.f;
  }
  if (g_frameBufferScale == scale) {
    return;
  }
  g_frameBufferScale = scale;
  request_frame_buffer_resize();
}

void set_frame_buffer_aspect_fit(bool fit) {
  if (g_frameBufferAspectFit == fit) {
    return;
  }

  g_frameBufferAspectFit = fit;
  request_frame_buffer_resize();
}

void set_background_input(bool value) { SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, value ? "1" : "0"); }

} // namespace aurora::window

#if defined(SDL_PLATFORM_ANDROID)
extern "C" JNIEXPORT void JNICALL Java_dev_encounter_aurora_AuroraSurface_nativeSetSurfaceReady(JNIEnv*, jclass,
                                                                                                jboolean ready) {
  aurora::window::set_surface_ready(ready == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL Java_dev_encounter_aurora_AuroraSurface_nativeSetSurfaceChanging(JNIEnv*, jclass,
                                                                                                   jboolean changing) {
  aurora::window::set_surface_changing(changing == JNI_TRUE);
}

// Called from surfaceDestroyed after the surface was marked not ready: the main
// thread's next frame drops the swapchain. Presenting to a destroyed window can
// lose the device on some drivers, and a lost device is fatal. The timeout keeps
// a busy main thread (a long load) from turning this into an ANR.
extern "C" JNIEXPORT jboolean JNICALL Java_dev_encounter_aurora_AuroraSurface_nativeWaitSurfaceReleased(JNIEnv*,
                                                                                                       jclass) {
  const bool released = aurora::window::wait_surface_released(2000);
  if (!released) {
    aurora::window::Log.warn("surfaceDestroyed: renderer still holds the surface after 2 s");
  }
  return released ? JNI_TRUE : JNI_FALSE;
}
#endif
