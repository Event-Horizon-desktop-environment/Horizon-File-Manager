#pragma once

#include <string>

namespace eh::file_browser {

struct AppState;

/// Create the inotify descriptor backing the directory watcher. Returns the
/// fd on success or -1 if inotify is unavailable on this system.
int dir_watch_init();

/// Re-arm directory watches so they track whichever folders the active tab
/// and (when split) the right pane are currently viewing. Cheap: no-op when
/// the watched paths are unchanged. Returns true once a watch covers the
/// active tab's folder (events this loop should be respected).
bool dir_watch_sync(AppState& app);

/// Drain any pending inotify events. On structural changes (create/delete/
/// move) it flags that the pane's listing must be fully reloaded; on
/// modify/attribute/close_write it records the affected child names so
/// dir_watch_refresh_entries can re-stat them in place.
///   fresh: true if dir_watch_fd became readable this iteration.
///   need_reload_tab / need_reload_pane: set when that pane needs reload_dir.
void dir_watch_process(AppState& app, bool fresh, bool& need_reload_tab,
                       bool& need_reload_pane);

/// Re-stat the named children of the active tab's folder and update their
/// cached FileEntry metadata (size, mtime, mode, type, mime, icon) in place,
/// so the display reflects the current file without a full directory reload.
/// Returns true if any entries were refreshed (caller should redraw).
bool dir_watch_refresh_entries(AppState& app);

/// Close the inotify descriptor (idempotent). Called at app teardown.
void dir_watch_close(AppState& app);

}  // namespace eh::file_browser
