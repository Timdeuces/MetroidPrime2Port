if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

set(_kmsdrm_video "${SDL_SOURCE_DIR}/src/video/kmsdrm/SDL_kmsdrmvideo.c")
if (NOT EXISTS "${_kmsdrm_video}")
  message(FATAL_ERROR "SDL KMSDRM source is missing: ${_kmsdrm_video}")
endif ()

file(READ "${_kmsdrm_video}" _kmsdrm_source)
string(FIND "${_kmsdrm_source}" "KMSDRM: EGL/GLES initialization failed for requested profile" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL3 KMSDRM EGL diagnostics patch already applied")
  return()
endif ()

set(_old [=[            if (!SDL_EGL_LoadLibrary(_this, NULL, egl_display, EGL_PLATFORM_GBM_MESA)) {
                // Try again with OpenGL ES 2.0
                _this->gl_config.profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
                _this->gl_config.major_version = 2;
                _this->gl_config.minor_version = 0;
                if (!SDL_EGL_LoadLibrary(_this, NULL, egl_display, EGL_PLATFORM_GBM_MESA)) {
                    return SDL_SetError("Can't load EGL/GL library on window creation.");
                }
            }
]=])

set(_new [=[            if (!SDL_EGL_LoadLibrary(_this, NULL, egl_display, EGL_PLATFORM_GBM_MESA)) {
                char requested_error[256];
                const int requested_major = _this->gl_config.major_version;
                const int requested_minor = _this->gl_config.minor_version;
                SDL_snprintf(requested_error, sizeof(requested_error), "%s", SDL_GetError());
                SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                             "KMSDRM: EGL/GLES initialization failed for requested profile %d.%d: %s",
                             requested_major, requested_minor, requested_error);

                // Retry with OpenGL ES 2.0, preserving the original EGL failure for diagnostics.
                _this->gl_config.profile_mask = SDL_GL_CONTEXT_PROFILE_ES;
                _this->gl_config.major_version = 2;
                _this->gl_config.minor_version = 0;
                if (!SDL_EGL_LoadLibrary(_this, NULL, egl_display, EGL_PLATFORM_GBM_MESA)) {
                    char fallback_error[256];
                    SDL_snprintf(fallback_error, sizeof(fallback_error), "%s", SDL_GetError());
                    SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                                 "KMSDRM: EGL/GLES initialization failed for ES 2.0 fallback: %s",
                                 fallback_error);
                    return SDL_SetError("Can't load EGL/GL library on window creation "
                                        "(requested GLES %d.%d: %s; GLES 2.0 fallback: %s).",
                                        requested_major, requested_minor, requested_error,
                                        fallback_error);
                }
            }
]=])

string(FIND "${_kmsdrm_source}" "${_old}" _old_pos)
if (_old_pos EQUAL -1)
  message(FATAL_ERROR "SDL3 KMSDRM EGL diagnostics patch: target block not found")
endif ()

string(REPLACE "${_old}" "${_new}" _kmsdrm_source "${_kmsdrm_source}")
file(WRITE "${_kmsdrm_video}" "${_kmsdrm_source}")
message(STATUS "Applied SDL3 KMSDRM EGL diagnostics patch")