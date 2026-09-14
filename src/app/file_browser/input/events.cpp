// events.cpp — input-module shared helper.
//
// The event entry points (handle_click, handle_key, handle_scroll,
// handle_pointer_move, handle_pointer_release, properties_hit_test,
// settings_hit_test) were moved to per-handler TUs; see events.hpp for the
// layout. events.cpp now owns the single helper two handlers share:
// apply_scrollbar_drag (grabbed by handle_click on a click inside the
// scrollbar strip, driven by handle_pointer_move during the drag).

#include "events.hpp"

#include <algorithm>
#include <cmath>

namespace eh::file_browser {

// ── Main-view scrollbar dragging ────────────────────────────────────
//
// draw_scrollbar() records its interactive rect every frame; clicking inside
// that strip grabs the thumb (or jumps to the tapped position) and motion
// maps pointer Y to scroll position, bypassing the smooth-scroll animation.
void apply_scrollbar_drag(AppState& app, int y) {
  const auto& sb = app.scrollbar_drag_rect;
  double thumb_h = std::max(static_cast<double>(sb.view_h) * sb.view_h /
                                static_cast<double>(sb.content_h),
                            20.0);
  int max_scroll = std::max(0, sb.content_h - sb.view_h);
  double denom = std::max(1.0, static_cast<double>(sb.h) - thumb_h);
  double frac = (static_cast<double>(y - sb.y) - app.scrollbar_grab_dy) / denom;
  frac = std::clamp(frac, 0.0, 1.0);
  int v = static_cast<int>(std::lround(frac * max_scroll));
  if (sb.computer) {
    app.computer_scroll_px = v;
    app.computer_scroll_smooth_current = static_cast<double>(v);
    app.computer_scroll_smooth_target = static_cast<double>(v);
  } else {
    auto& tab = app.cur_tab();
    tab.scroll_px = v;
    tab.scroll_smooth_current = static_cast<double>(v);
    tab.scroll_smooth_target = static_cast<double>(v);
  }
}

} // namespace eh::file_browser