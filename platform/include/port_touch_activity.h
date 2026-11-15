#pragma once

#include <SDL2/SDL.h>
#include <atomic>
#include <cstdlib>

namespace PortTouch {
// Last-used input, not the saved touch-controls setting or pad connection state.
class Activity {
  std::atomic<bool> mTouch{false};
public:
  void Touch() { mTouch.store(true, std::memory_order_release); }
  void Physical() { mTouch.store(false, std::memory_order_release); }
  bool Active(bool available, bool overlay) const {
    return available && !overlay && mTouch.load(std::memory_order_acquire);
  }
};

// The caller supplies virtual-device identity only for gamepad events. Touch's
// virtual pad and SDL's synthetic mouse events must not switch back to physical.
inline bool PhysicalEvent(const SDL_Event& event, bool overlay, bool virtualPad) {
  switch (event.type) {
  case SDL_CONTROLLERBUTTONDOWN: return !virtualPad;
  case SDL_CONTROLLERAXISMOTION:
    return !virtualPad && std::abs(int(event.caxis.value)) > 16000;
  case SDL_MOUSEBUTTONDOWN:
    return event.button.which != SDL_TOUCH_MOUSEID;
  case SDL_KEYDOWN:
    if (event.key.repeat || overlay) return false;
    switch (event.key.keysym.scancode) {
    case SDL_SCANCODE_AC_BACK:
    case SDL_SCANCODE_VOLUMEUP:
    case SDL_SCANCODE_VOLUMEDOWN:
    case SDL_SCANCODE_MUTE:
    case SDL_SCANCODE_AUDIOPLAY:
    case SDL_SCANCODE_AUDIOSTOP:
    case SDL_SCANCODE_POWER: return false;
    default: return true;
    }
  default: return false;
  }
}
} // namespace PortTouch
