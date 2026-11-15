#pragma once

#include <chrono>
#include <cstdint>

namespace PortTime {
inline uint64_t NowNs() noexcept {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
} // namespace PortTime