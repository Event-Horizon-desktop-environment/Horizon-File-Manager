#pragma once

#include "../../app.hpp"

namespace eh::file_browser {

// ── nav.cpp shared internals ─────────────────────────────────────
// Helpers defined in nav.cpp but consumed by sibling modules of the
// navigation/scan/preview split:
//
//   is_hidden_file          scan.cpp, preview_popup.cpp (hidden entries)
//   read_hidden_file        app.hpp (already public) — scan, popup
//   reset_scroll_and_selection   scan.cpp (apply_scan_result re-sets the
//                           filtered view after a directory reload)
//   matches_filter          preview_popup.cpp (space-preview honors the
//                           active filter)
//   query_matches_entry     scan.cpp (apply_scan_result filters each row)

bool is_hidden_file(const std::string& name);
void reset_scroll_and_selection(AppState& app);
bool matches_filter(const AppState& app, const FileEntry& entry);
bool query_matches_entry(const AppState& app, const std::string& name);

} // namespace eh::file_browser