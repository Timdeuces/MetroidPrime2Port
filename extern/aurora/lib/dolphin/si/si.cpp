#include <dolphin/si.h>
#include "../../input.hpp"

uint32_t SIProbe(int32_t chan) {
  auto* const controller = aurora::input::get_controller_for_player(chan);
  if (controller == nullptr) {
    return SI_ERROR_NO_RESPONSE;
  }

  return SI_GC_CONTROLLER;
}
