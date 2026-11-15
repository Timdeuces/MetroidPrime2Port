if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

set(_kmsdrm_video "${SDL_SOURCE_DIR}/src/video/kmsdrm/SDL_kmsdrmvideo.c")
set(_kmsdrm_gles "${SDL_SOURCE_DIR}/src/video/kmsdrm/SDL_kmsdrmopengles.c")
if (NOT EXISTS "${_kmsdrm_video}" OR NOT EXISTS "${_kmsdrm_gles}")
  message(FATAL_ERROR "SDL KMSDRM sources are missing: ${_kmsdrm_video}, ${_kmsdrm_gles}")
endif ()

file(READ "${_kmsdrm_video}" _kmsdrm_source)
file(READ "${_kmsdrm_gles}" _kmsdrm_gles_source)
string(FIND "${_kmsdrm_gles_source}" "info.src_w = KMSDRM_gbm_bo_get_width(windata->next_bo);" _source_size_patched)
if (_source_size_patched EQUAL -1)
  set(_old_source_size [=[    info.src_w = window->w;  // !!! FIXME: was windata->src_w in the original atomic patch
    info.src_h = window->h;  // !!! FIXME: was windata->src_h in the original atomic patch
]=])
  set(_new_source_size [=[    // The DRM source rectangle must match the locked framebuffer, not SDL's logical window size.
    info.src_w = KMSDRM_gbm_bo_get_width(windata->next_bo);
    info.src_h = KMSDRM_gbm_bo_get_height(windata->next_bo);
]=])
  string(FIND "${_kmsdrm_gles_source}" "${_old_source_size}" _source_size_pos)
  if (_source_size_pos EQUAL -1)
    message(FATAL_ERROR "SDL3 KMSDRM atomic diagnostics patch: source-size target not found")
  endif ()
  string(REPLACE "${_old_source_size}" "${_new_source_size}" _kmsdrm_gles_source "${_kmsdrm_gles_source}")
  message(STATUS "Applied SDL3 KMSDRM GBM source-size fix")
else ()
  message(STATUS "SDL3 KMSDRM GBM source-size fix already applied")
endif ()

string(FIND "${_kmsdrm_source}" "KMSDRM atomic commit ioctl failed" _diagnostics_patched)
if (_diagnostics_patched EQUAL -1)
  set(_old [=[    ret = KMSDRM_drmModeAtomicCommit(viddata->drm_fd, dispdata->atomic_req,
              atomic_flags, NULL);

    if (ret) {
        SDL_SetError("Atomic commit failed, returned %d.", ret);
]=])

  set(_new [=[    ret = KMSDRM_drmModeAtomicCommit(viddata->drm_fd, dispdata->atomic_req,
              atomic_flags, NULL);

    if (ret) {
        const int commit_errno = errno;
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO,
                     "KMSDRM atomic commit ioctl failed: ret=%d errno=%d (%s) "
                     "drm_fd=%d flags=0x%x blocking=%d allow_modeset=%d",
                     ret, commit_errno, strerror(commit_errno), viddata->drm_fd,
                     atomic_flags, blocking, allow_modeset);
        SDL_SetError("Atomic commit failed, returned %d (errno=%d: %s).",
                     ret, commit_errno, strerror(commit_errno));
]=])

  string(FIND "${_kmsdrm_source}" "${_old}" _old_pos)
  if (_old_pos EQUAL -1)
    message(FATAL_ERROR "SDL3 KMSDRM atomic diagnostics patch: target block not found")
  endif ()

  string(REPLACE "${_old}" "${_new}" _kmsdrm_source "${_kmsdrm_source}")
  message(STATUS "Applied SDL3 KMSDRM atomic commit diagnostics patch")
else ()
  message(STATUS "SDL3 KMSDRM atomic commit diagnostics patch already applied")
endif ()

file(WRITE "${_kmsdrm_video}" "${_kmsdrm_source}")
file(WRITE "${_kmsdrm_gles}" "${_kmsdrm_gles_source}")
