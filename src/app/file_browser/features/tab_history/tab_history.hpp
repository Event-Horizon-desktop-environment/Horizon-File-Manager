#pragma once

#include "app/file_browser/app_types.hpp"

namespace eh::file_browser {

// Snapshot of a recently closed tab, kept so it can be restored
void remember_closed_tab(AppState& app, const Tab& tab);

// Reopen the most recently closed tab. Returns false when the history is
// empty.
bool reopen_last_closed_tab(AppState& app);

// Session snapshot text (toml): tabs, active index, split + right path.
// Pure: probes assert the exact bytes.
std::string session_snapshot(AppState& app);

// Write the snapshot to session.toml (unconditional).
void session_write(AppState& app);

// Throttled autosave from draw(): at most every 5 s, only on change.
void session_save_maybe(AppState& app);

// Restore tabs/split from session.toml. No-op (false) when the file is
// missing/empty or no stored path still exists. Navigates the active tab;
// inactive tabs load on activation (activation always reloads).
bool session_restore(AppState& app);

} // namespace eh::file_browser
