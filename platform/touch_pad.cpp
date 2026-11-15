#include "touch_pad.h"

#include <cmath>

namespace PortTouchPad {

void Describe(SDL_VirtualJoystickDesc& desc) {
  desc = {};
  desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
  desc.naxes = SDL_CONTROLLER_AXIS_MAX;
  desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
  // Report as a common Xbox pad so the pad type, and with it the port's prompt
  // icons, match what the overlay draws. These ids are cosmetic: a virtual
  // device's GUID uses the virtual bus, so they do not select a built-in
  // mapping. See the header for why no mapping string is installed.
  desc.vendor_id = 0x045e;
  desc.product_id = 0x02ea;
  desc.name = "Metroid Prime touch gamepad";
}

Sint16 AxisValue(float deflection) {
  const float clamped = deflection < -1.f ? -1.f : (deflection > 1.f ? 1.f : deflection);
  const float scaled = clamped < 0.f ? clamped * 32768.f : clamped * 32767.f;
  return static_cast< Sint16 >(std::lround(scaled));
}

Pad Attach() {
  Pad pad;
  SDL_VirtualJoystickDesc desc;
  Describe(desc);
  pad.id = SDL_JoystickAttachVirtualEx(&desc);
  if (pad.id < 0)
    return pad;

  // No SDL_AddGamepadMapping here on purpose; the header explains why.
  pad.handle = SDL_JoystickOpen(pad.id);
  if (pad.handle == nullptr) {
    // See the header: dropping the id would leak a device per call.
    SDL_JoystickDetachVirtual(pad.id);
    pad.id = -1;
  }
  return pad;
}

void Detach(Pad& pad) {
  if (pad.handle != nullptr) {
    SDL_JoystickClose(pad.handle);
    pad.handle = nullptr;
  }
  if (pad.id >= 0) {
    SDL_JoystickDetachVirtual(pad.id);
    pad.id = -1;
  }
}

} // namespace PortTouchPad
