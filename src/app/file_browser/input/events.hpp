#pragma once

#include "../app.hpp"

namespace eh::file_browser {

// ── Input module (input/*.cpp) ───────────────────────────────────
// Shared input-module surface. The event entry points (handle_click,
// handle_key, handle_scroll, handle_pointer_move, handle_pointer_release,
// properties_hit_test, settings_hit_test) are declared in app.hpp and
// implemented in frequency/called per-handler TUs:
//
//   click.cpp    handle_click, properties_hit_test, settings_hit_test,
//                modal_blocks_content
//   keyboard.cpp handle_key + grouped-header row helpers
//   pointer.cpp  handle_pointer_move
//   scroll.cpp   handle_scroll
//   release.cpp  handle_pointer_release
//   events.cpp   apply_scrollbar_drag (shared by click + pointer)

// Constants matching draw.cpp for filter dropdown layout (used by both the
// click and pointer handlers so the two can never disagree about the dropdown
// geometry).
static constexpr int kFilterHdrH = 28;
static constexpr int kFilterItemH = 24;
static constexpr int kFilterPD = 6;
static constexpr int kFilterSep = 4;

// Main-view scrollbar: maps pointer Y to scroll position (thumb grab /
// tap-to-jump). Called by handle_click (initial grab/tap) and
// handle_pointer_move (thumb drag); the grab offset captured at grab time is
// applied here. Defined in events.cpp.
void apply_scrollbar_drag(AppState& app, int y);

} // namespace eh::file_browser