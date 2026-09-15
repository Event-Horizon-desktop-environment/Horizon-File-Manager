// layout.hpp — SINGLE source of truth for pane geometry.
//
// This is the one place the file browser derives grid/list layout. Draw,
// hit-test, and keyboard navigation all call compute_grid_layout() /
// compute_list_layout() instead of re-deriving constants on their own — a
// geometry bug is therefore fixed exactly once, in exactly one function,
// and can never silently drift out of sync between "what I see" and
// "what I click".
//
// The one calculation that historically caused that drift is preserved here
// and lives ONLY here: draw_grid_view snaps the icon size down to the
// icon cache's render-size bucket (IconCache::bucket_down) so the raster is
// exact-fit, but hit_test_grid used the raw, unsnapped value — so row/column
// geometry differed by the snap delta and the mismatch accumulated down the
// grid. Now that snap happens in exactly one place and every consumer sees
// the same numbers.
#pragma once

#include <algorithm>

struct AppState;
struct Tab;

namespace eh::file_browser {

// Pane geometry: which pane is under a pixel (draw + hit-test) or which
// pixel-independent layout a pane index owns (keyboard nav). Lives here so
// every consumer derives content width through the SAME math — a stale pane
// rect is the same disease as a stale row_h.
struct PaneViewRect {
  int x, y, w, h;
  int pane; // 0 = left/single pane, 1 = right split pane
};

// Rect of the pane under pixel px (click/touch path).
PaneViewRect pane_view_rect_at(const AppState& app, int px);

// Rect of pane index `pane` directly (keyboard path — focus is app.active_pane).
PaneViewRect pane_view_rect_for_index(const AppState& app, int pane);

// Tab whose entry list lives in the pane under pixel px. For split view the
// pane is derived FROM THE POINTER X, so this stays position-correct no matter
// which pane currently has focus; outside split view it is the active tab.
Tab& pane_tab_at(AppState& app, int px);

// Pane-local grid geometry, computed from THIS pane's content width and the
// current zoom. Never stored on AppState — the per-pane values recompute on
// every demand so split view can never hand out a stale "last pane drew"
// answer.
struct GridLayout {
  int min_cell_w;
  int col_gap;
  int row_gap;
  int cols;

  int cell_w;
  int icon_size; // bucket_down applied — matches the icon cache raster exactly
  int icon_area; // = icon_size (the square region the icon blits into)
  int line_h;    // measured per-line text height (real Pango line height, not
                 // a guess) so a 2-line wrapped name fits the label box
  int label_h;   // 2 lines of label text under the icon (+2px pad for the
                 // raster's descender margin)
  int text_gap;  // tight: label sits close under the icon
  int item_h;    // icon_area + text_gap + label_h
  int row_h;     // item_h + row_gap
  int row_gap_at_top; // row_gap (first-row inset handled by caller as before)

  int grid_w;        // cols*cell_w + (cols-1)*col_gap
  int grid_offset_x; // (content_w - grid_w) / 2 — horizontal centering
};

// List-view geometry — currently just entry_height, kept as a single type so
// callers never mix list row math with grid row math.
struct ListLayout {
  int entry_h;
  int col_header_h;
};

// Pure, deterministic, side-effect-free. content_w is the pane's usable
// content width; zf is zoom_pct/100.0. Byte-identical to what draw_grid_view
// draws, because this is the code draw grid view used to hold.
GridLayout compute_grid_layout(int content_w, double zf);

ListLayout compute_list_layout(double zf);

} // namespace eh::file_browser
