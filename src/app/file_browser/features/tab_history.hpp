#pragma once

#include "app/file_browser/app_types.hpp"

namespace eh::file_browser {

// Snapshot of a recently closed tab, kept so it can be restored
// (AppState::ClosedTab).
void remember_closed_tab(AppState& app, const Tab& tab);

// Reopen the most recently closed tab. Returns false when the history is
// empty.
bool reopen_last_closed_tab(AppState& app);

} // namespace eh::file_browser
