if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

set(_egl_source "${SDL_SOURCE_DIR}/src/video/SDL_egl.c")
if (NOT EXISTS "${_egl_source}")
  message(FATAL_ERROR "SDL EGL source is missing: ${_egl_source}")
endif ()

file(READ "${_egl_source}" _source)
string(FIND "${_source}" "SDL EGL GBM request:" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL EGL platform diagnostics patch already applied")
  return()
endif ()

function (_sdl_replace _old _new _description)
  string(FIND "${_source}" "${_old}" _position)
  if (_position EQUAL -1)
    message(FATAL_ERROR "SDL EGL platform diagnostics: ${_description} not found")
  endif ()
  string(REPLACE "${_old}" "${_new}" _updated "${_source}")
  set(_source "${_updated}" PARENT_SCOPE)
endfunction ()

_sdl_replace([=[    if (platform) {
        /* EGL 1.5 allows querying for client version with EGL_NO_DISPLAY
]=] [=[    if (platform) {
        const char* client_extensions = _this->egl_data->eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        SDL_Log("SDL EGL GBM request: nativeDisplay=%p platform=0x%x EGL=%d.%d clientExtensions=%s",
                (void *)(uintptr_t)native_display, platform, _this->egl_data->egl_version_major,
                _this->egl_data->egl_version_minor,
                client_extensions != NULL ? client_extensions : "(unavailable)");
        /* EGL 1.5 allows querying for client version with EGL_NO_DISPLAY
]=] "GBM request log")

_sdl_replace([=[            _this->egl_data->egl_display = _this->egl_data->eglGetPlatformDisplay(platform, (void *)(uintptr_t)native_display, attribs);
]=] [=[            SDL_Log("SDL EGL GBM route: using eglGetPlatformDisplay");
            _this->egl_data->egl_display = _this->egl_data->eglGetPlatformDisplay(platform, (void *)(uintptr_t)native_display, attribs);
            SDL_Log("SDL EGL GBM route: eglGetPlatformDisplay returned %p",
                    (void *)(uintptr_t)_this->egl_data->egl_display);
]=] "EGL 1.5 platform route")

_sdl_replace([=[                    _this->egl_data->egl_display = _this->egl_data->eglGetPlatformDisplayEXT(platform, (void *)(uintptr_t)native_display, NULL);
]=] [=[                    SDL_Log("SDL EGL GBM route: using eglGetPlatformDisplayEXT");
                    _this->egl_data->egl_display = _this->egl_data->eglGetPlatformDisplayEXT(platform, (void *)(uintptr_t)native_display, NULL);
                    SDL_Log("SDL EGL GBM route: eglGetPlatformDisplayEXT returned %p",
                            (void *)(uintptr_t)_this->egl_data->egl_display);
]=] "EGL extension platform route")

_sdl_replace([=[        } else {
            if (SDL_EGL_HasExtension(_this, SDL_EGL_CLIENT_EXTENSION, "EGL_EXT_platform_base")) {
]=] [=[        } else {
            SDL_Log("SDL EGL GBM route: core eglGetPlatformDisplay unavailable; checking EGL_EXT_platform_base");
            if (SDL_EGL_HasExtension(_this, SDL_EGL_CLIENT_EXTENSION, "EGL_EXT_platform_base")) {
]=] "EGL extension route selection")

_sdl_replace([=[    if ((_this->egl_data->egl_display == EGL_NO_DISPLAY) &&
        (_this->egl_data->eglGetDisplay) &&
        SDL_GetHintBoolean(SDL_HINT_VIDEO_EGL_ALLOW_GETDISPLAY_FALLBACK, true)) {
        _this->egl_data->egl_display = _this->egl_data->eglGetDisplay(native_display);
    }
]=] [=[    if (_this->egl_data->egl_display == EGL_NO_DISPLAY && _this->egl_data->eglGetDisplay) {
        const bool allow_fallback = SDL_GetHintBoolean(SDL_HINT_VIDEO_EGL_ALLOW_GETDISPLAY_FALLBACK, true);
        SDL_Log("SDL EGL GBM route: eglGetDisplay fallback %s",
                allow_fallback ? "enabled" : "disabled by hint");
        if (allow_fallback) {
            _this->egl_data->egl_display = _this->egl_data->eglGetDisplay(native_display);
            SDL_Log("SDL EGL GBM route: eglGetDisplay returned %p",
                    (void *)(uintptr_t)_this->egl_data->egl_display);
        }
    }
]=] "eglGetDisplay fallback log")

_sdl_replace([=[    if (_this->egl_data->egl_display == EGL_NO_DISPLAY) {
        _this->gl_config.driver_loaded = 0;
]=] [=[    SDL_Log("SDL EGL GBM route: selected EGLDisplay=%p",
            (void *)(uintptr_t)_this->egl_data->egl_display);
    if (_this->egl_data->egl_display == EGL_NO_DISPLAY) {
        _this->gl_config.driver_loaded = 0;
]=] "selected EGL display log")

file(WRITE "${_egl_source}" "${_source}")
message(STATUS "Applied SDL EGL platform diagnostics patch")