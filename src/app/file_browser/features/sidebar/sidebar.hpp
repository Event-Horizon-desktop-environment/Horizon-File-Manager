#pragma once

#include "app/file_browser/app_types.hpp"

#include <cairo/cairo.h>

#include <string>

namespace eh::file_browser {

// Every piece of file-browser sidebar logic and geometry lives here so the
// width the LAYOUT contributes and the width the PAINTER draws can never
// drift apart: they all derive the sidebar's layout width from
// app.sidebar_w() (0 while the sidebar is folded), never from the raw
// app.sidebar_width. A future edit that reintroduces the old raw-width
// mismatch would have to do it inside this one file.

/// Content column geometry — the only place the sidebar's contribution to
/// layout is computed. Uses the fold-aware app.sidebar_w(), so the content
/// column expands into the sidebar's space the moment it folds.
void sidebar_content_geometry(AppState& app, int& cx, int& cy, int& cw,
                              int& ch, int& banner_h);

/// Recompute the narrow-window fold for width `width`; resets the
/// fold-flap reveal whenever the fold state flips.
void update_sidebar_fold(AppState& app, int width);

/// Paint the folded-sidebar flap (a temporary overlay above the content
/// column). No-op unless the sidebar is folded AND revealed.
void paint_sidebar_flap(AppState& app, cairo_t* cr, int w, int h, int view_h,
                        int status_h);

/// Paint the inline (expanded) sidebar column: background, items and the
/// separator / resize handle. Computes its own width from app.sidebar_w().
void paint_inline_sidebar(AppState& app, cairo_t* cr, int h, int status_h,
                          int view_h);

/// Byte-size formatter ("463.2 GB"). Shared by the sidebar's drive usage
/// rows and draw.cpp's list/status views (defined here with the sidebar,
/// declared in app.hpp).
std::string format_size(uint64_t bytes);

/// Pinned sidebar scale: paint, hit-testing and scroll math all derive
/// from it, so rows can never drift out from under the scroll clamp.
inline constexpr double kSidebarZf = 1.2;

/// True for drive rows with a usage bar (taller rows).
inline bool sidebar_has_usage(const SidebarLocation& loc) {
  return (loc.kind == SidebarLocation::Kind::Drive ||
          loc.kind == SidebarLocation::Kind::Root) &&
         loc.total_bytes > 0 && loc.is_mounted;
}

/// Pixel height of one sidebar row (usage rows are taller).
inline int sidebar_row_h(const SidebarLocation& loc) {
  int item_h = static_cast<int>(36.0 * kSidebarZf);
  if (sidebar_has_usage(loc))
    item_h = static_cast<int>(52.0 * kSidebarZf);
  return item_h;
}

/// Section boundaries + exact pixel geometry, computed once per model and
/// shared by paint, hit-testing and the scroll clamp — the single source
/// of truth that keeps the sidebar's bottom reachable (Dolphin/Nautilus
/// parity: overflow scrolls instead of clipping away).
struct SidebarLayout {
  int places_end = 0;
  int fav_start = 0;
  int network_end = 0;
  int drives_start = 0;
  int searches_start = 0; // saved searches (end); == total when empty
  int total = 0;
  int header_h = 0; // 24px section titles
  int div_h = 0;    // 16 + 1 + 16 divider
  int top_pad = 0;  // 24px top padding
  int content_h = 0; // total, top pad included
  /// Unscrolled y of a row top (row must be a real item index).
  int row_y(const std::vector<SidebarLocation>& locs, int idx) const;
};

/// Compute section bounds + content height for a location list.
SidebarLayout sidebar_layout(const std::vector<SidebarLocation>& locs);

/// True when (x, y) is inside the fold-toggle toolbar button (drawn only
/// while the sidebar is folded).
bool sidebar_toggle_hit(AppState& app, int x, int y);

/// Toggle the fold-flap reveal (toolbar toggle button click).
void toggle_sidebar_flap(AppState& app);

/// Dismiss the fold flap when a click lands outside it (and not on the
/// toggle button). The click is still processed normally afterwards.
void dismiss_sidebar_flap(AppState& app, int x, int y);

/// Begin a sidebar width drag (pointer pressed within the separator handle).
/// Returns true when a drag actually started (caller should consume the
/// press).
bool begin_sidebar_resize(AppState& app, int x);

/// Continue a sidebar width drag. Clamps to [120, 400] and keeps the
/// zoom-independent width base in sync.
void drag_sidebar_resize(AppState& app, int x);

/// Release the sidebar resize drag.
void end_sidebar_resize(AppState& app);

/// Begin a scrollbar thumb drag (pointer pressed on the thumb).
/// Returns true when a drag actually started (caller should consume it).
bool begin_sidebar_scroll(AppState& app, int x, int y);

/// Continue a scrollbar thumb drag.
void drag_sidebar_scroll(AppState& app, int y);

/// Release the scrollbar drag.
void end_sidebar_scroll(AppState& app);

/// Track the separator resize-edge hover. Returns true when the flag
/// changed (caller triggers a repaint on change).
bool update_sidebar_resize_hover(AppState& app, int x);

// size_sidebar_to_content / draw_sidebar / hit_test_sidebar /
// hit_test_fav_section / refresh_sidebar are implemented here and stay
// declared in app.hpp with the rest of the shared file-browser API.

} // namespace eh::file_browser