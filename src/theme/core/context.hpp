#pragma once

#include <cstdint>

namespace m3 {

// Per-frame rendering context propagated to all widgets.
// Widgets use ctx.scale to multiply every dp/sp value into physical pixels.
struct ThemeContext {
  double scale = 1.0;        // zoom_pct * dpi_factor / 100
  double dpi_factor = 1.0;   // from Wayland fractional-scale protocol
  uint64_t now_ms = 0;       // timestamp for animations

  [[nodiscard]] double effective_scale() const noexcept {
    return scale * dpi_factor;
  }
};

} // namespace m3
