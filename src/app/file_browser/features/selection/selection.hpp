#pragma once

#include <cairo/cairo.h>

#include "app/file_browser/app_types.hpp"

namespace eh::file_browser {

// Toggle every visible entry's membership in the multi-selection.
void invert_selection(AppState& app);

// ── Select-by-pattern dialog (glob match against visible entries) ──

void open_select_pattern(AppState& app);
void close_select_pattern(AppState& app);

// Add every visible entry whose filename matches the current pattern buffer
// to the multi-selection, then close the dialog.
void apply_select_pattern(AppState& app);

void draw_select_pattern_dialog(AppState& app, cairo_t* cr);

// Consume a key press while the dialog is open. Returns true when handled.
bool handle_select_pattern_key(AppState& app, uint32_t keysym,
                               const char* utf8, int utf8_len);

// Consume a mouse button press while the dialog is open. Returns true when
// handled (including clicks on the dimmed backdrop, which cancel).
bool handle_select_pattern_click(AppState& app, int button, int x, int y);

} // namespace eh::file_browser
