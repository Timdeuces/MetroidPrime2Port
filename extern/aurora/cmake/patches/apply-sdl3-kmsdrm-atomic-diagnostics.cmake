if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

set(_kmsdrm_video "${SDL_SOURCE_DIR}/src/video/kmsdrm/SDL_kmsdrmvideo.c")
if (NOT EXISTS "${_kmsdrm_video}")
  message(FATAL_ERROR "SDL KMSDRM source is missing: ${_kmsdrm_video}")
endif ()

file(READ "${_kmsdrm_video}" _kmsdrm_source)
string(FIND "${_kmsdrm_source}" "KMSDRM atomic commit ioctl failed" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL3 KMSDRM atomic commit diagnostics patch already applied")
  return()
endif ()

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
file(WRITE "${_kmsdrm_video}" "${_kmsdrm_source}")
message(STATUS "Applied SDL3 KMSDRM atomic commit diagnostics patch")
