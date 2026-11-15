#include "BackendBinding.hpp"

#include <aurora/webgpu.hpp>

#include <memory>
#include <cstring>

#include "../window.hpp"

#if !defined(__APPLE__) && !defined(__ANDROID__)
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_vulkan.h>
#endif

namespace aurora::webgpu {
namespace utils {

std::shared_ptr<wgpu::ChainedStruct> SetupWindowAndGetSurfaceDescriptorCocoa(SDL_Window* window);

#if defined(__linux__)
WGPUBool create_kmsdrm_surface(void* userdata, void* instance, void** surface) {
  static bool vulkanLoaderInitialized = false;
  if (!vulkanLoaderInitialized) {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
      SDL_Log("KMSDRM: SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
      return WGPU_FALSE;
    }
    vulkanLoaderInitialized = true;
  }

  VkSurfaceKHR nativeSurface{};
  if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(userdata), reinterpret_cast<VkInstance>(instance),
                                &nativeSurface)) {
    SDL_Log("KMSDRM: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
    return WGPU_FALSE;
  }

  *surface = reinterpret_cast<void*>(nativeSurface);
  return WGPU_TRUE;
}

#if defined(WEBGPU_DAWN)
WGPUBool get_kmsdrm_egl_surface(void* userdata, void** surface) {
  auto* window = static_cast<SDL_Window*>(userdata);
  void* eglSurface = window::get_kmsdrm_egl_surface(window);
  if (eglSurface == nullptr) {
    SDL_Log("KMSDRM: no SDL-created EGL draw surface is available for Dawn");
    return WGPU_FALSE;
  }
  *surface = eglSurface;
  return WGPU_TRUE;
}

WGPUBool present_kmsdrm_egl_surface(void* userdata) {
  if (!window::make_kmsdrm_gl_context_current(true)) {
    SDL_Log("KMSDRM: failed to make SDL's GL context current for presentation: %s", SDL_GetError());
    return WGPU_FALSE;
  }
  SDL_GL_SwapWindow(static_cast<SDL_Window*>(userdata));
  const bool released = window::make_kmsdrm_gl_context_current(false);
  if (!released) {
    SDL_Log("KMSDRM: failed to release SDL's GL context after presentation: %s", SDL_GetError());
    return WGPU_FALSE;
  }
  return WGPU_TRUE;
}
#endif
#endif

std::shared_ptr<wgpu::ChainedStruct> SetupWindowAndGetSurfaceDescriptor(SDL_Window* window) {
#if defined(SDL_PLATFORM_MACOS) || defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_TVOS)
  return SetupWindowAndGetSurfaceDescriptorCocoa(window);
#else
#if defined(__ANDROID__)
  SDL_SysWMinfo wmInfo{};
  SDL_VERSION(&wmInfo.version);
  if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
    return nullptr;
  }
  std::shared_ptr<wgpu::SurfaceSourceAndroidNativeWindow> desc =
      std::make_shared<wgpu::SurfaceSourceAndroidNativeWindow>();
  desc->window = wmInfo.info.android.window;
  if (desc->window == nullptr) {
    return nullptr; // Port: no surface yet (or already destroyed); CreateAndroidSurfaceKHR would crash
  }
  return std::move(desc);
#elif defined(_WIN32)
  SDL_SysWMinfo wmInfo{};
  SDL_VERSION(&wmInfo.version);
  if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
    return nullptr;
  }
  std::shared_ptr<wgpu::SurfaceSourceWindowsHWND> desc = std::make_shared<wgpu::SurfaceSourceWindowsHWND>();
  desc->hwnd = wmInfo.info.win.window;
  desc->hinstance = wmInfo.info.win.hinstance;
  return std::move(desc);
#elif defined(__linux__)
  const char* driver = SDL_GetCurrentVideoDriver();
  if (driver != nullptr && std::strcmp(driver, "kmsdrm") == 0) {
    auto desc = std::make_shared<wgpu::SurfaceSourceKmsdrm>();
    desc->userdata = window;
    desc->createSurface = create_kmsdrm_surface;
  #if defined(WEBGPU_DAWN)
    desc->getEGLSurface = get_kmsdrm_egl_surface;
    desc->present = present_kmsdrm_egl_surface;
  #endif
    return desc;
  }
  SDL_SysWMinfo wmInfo{};
  SDL_VERSION(&wmInfo.version);
  if (!SDL_GetWindowWMInfo(window, &wmInfo)) {
    return nullptr;
  }
  if (wmInfo.subsystem == SDL_SYSWM_WAYLAND) {
    std::shared_ptr<wgpu::SurfaceSourceWaylandSurface> desc = std::make_shared<wgpu::SurfaceSourceWaylandSurface>();
    desc->display = wmInfo.info.wl.display;
    desc->surface = wmInfo.info.wl.surface;
    return std::move(desc);
  }
  if (wmInfo.subsystem == SDL_SYSWM_X11) {
    std::shared_ptr<wgpu::SurfaceSourceXlibWindow> desc = std::make_shared<wgpu::SurfaceSourceXlibWindow>();
    desc->display = wmInfo.info.x11.display;
    desc->window = wmInfo.info.x11.window;
    return std::move(desc);
  }
#endif
  return nullptr;
#endif
}

} // namespace utils

wgpu::Surface create_window_surface(const wgpu::Instance& instance, SDL_Window* window, const char* label) {
  if (!instance || window == nullptr) {
    return nullptr;
  }
  const auto chainedDescriptor = utils::SetupWindowAndGetSurfaceDescriptor(window);
  if (!chainedDescriptor) {
    return nullptr;
  }
  const wgpu::SurfaceDescriptor descriptor{
      .nextInChain = chainedDescriptor.get(),
      .label = label,
  };
  return instance.CreateSurface(&descriptor);
}

} // namespace aurora::webgpu
