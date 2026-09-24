#pragma once

#include "app/file_browser/app_types.hpp"

#include <cairo/cairo.h>

#include <string>

namespace eh::file_browser {

// ── Sidebar module (features/sidebar.cpp) ────────────────────────
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

/// Track the separator resize-edge hover. Returns true when the flag
/// changed (caller triggers a repaint on change).
bool update_sidebar_resize_hover(AppState& app, int x);

// size_sidebar_to_content / draw_sidebar / hit_test_sidebar /
// hit_test_fav_section / refresh_sidebar are implemented here and stay
// declared in app.hpp with the rest of the shared file-browser API.

} // namespace eh::file_browser