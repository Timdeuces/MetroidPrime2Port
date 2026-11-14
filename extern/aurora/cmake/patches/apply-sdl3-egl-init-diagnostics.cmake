if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

set(_egl_source "${SDL_SOURCE_DIR}/src/video/SDL_egl.c")
if (NOT EXISTS "${_egl_source}")
  message(FATAL_ERROR "SDL EGL source is missing: ${_egl_source}")
endif ()

file(READ "${_egl_source}" _source)
string(FIND "${_source}" "SDL EGL initialization failed (eglInitialize error" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL EGL initialization diagnostics patch already applied")
  return()
endif ()

set(_old [=[    if (_this->egl_data->eglInitialize(_this->egl_data->egl_display, NULL, NULL) != EGL_TRUE) {
        _this->gl_config.driver_loaded = 0;
        *_this->gl_config.driver_path = '\0';
        return SDL_SetError("Could not initialize EGL");
    }
]=])

set(_new [=[    if (_this->egl_data->eglInitialize(_this->egl_data->egl_display, NULL, NULL) != EGL_TRUE) {
        const EGLint egl_error = _this->egl_data->eglGetError();
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                     "SDL EGL initialization failed (eglInitialize error 0x%x)", egl_error);
        _this->gl_config.driver_loaded = 0;
        *_this->gl_config.driver_path = '\0';
        // Release the partial EGL state so KMSDRM can retry a lower GLES version.
        SDL_EGL_UnloadLibrary(_this);
        return SDL_SetError("Could not initialize EGL (eglInitialize error 0x%x)", egl_error);
    }
]=])

string(FIND "${_source}" "${_old}" _old_pos)
if (_old_pos EQUAL -1)
  message(FATAL_ERROR "SDL EGL initialization diagnostics patch: target block not found")
endif ()

string(REPLACE "${_old}" "${_new}" _source "${_source}")
file(WRITE "${_egl_source}" "${_source}")
message(STATUS "Applied SDL EGL initialization diagnostics patch")