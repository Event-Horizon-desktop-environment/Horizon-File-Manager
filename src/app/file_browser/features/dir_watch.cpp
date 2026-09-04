#include "app/file_browser/features/dir_watch.hpp"

#include "../app.hpp"
#include "../app_types.hpp"

#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

namespace eh::file_browser {

namespace {

// Structural events mean the listing itself changed (create/delete/move) —
// the pane gets a full reload_dir. The remaining event kinds are per-file
// metadata/content changes we can fold into the existing rows in place.
constexpr uint32_t kStructuralMask =
    IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
    IN_DELETE_SELF | IN_MOVE_SELF;

constexpr uint32_t kWatchMask =
    IN_ATTRIB | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MODIFY |
    IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF | IN_CLOSE_WRITE;

// Re-arm (or newly install) one watch. When the folder moved, drop the stale
// watch first so a vanished descriptor can't fire events for an old path.
void arm_watch(AppState& app, const std::string& path, int& wd,
               std::string& watched_path) {
  if (wd >= 0 && watched_path != path) {
    ::inotify_rm_watch(app.dir_watch_fd, wd);
    wd = -1;
    watched_path.clear();
  }
  if (path.empty() || wd >= 0) return;
  wd = ::inotify_add_watch(app.dir_watch_fd, path.c_str(), kWatchMask);
  if (wd >= 0) watched_path = path;
}

// Re-stat each named child of `tab` in place. Silently no-ops if the tab has
// navigated elsewhere (the queued names were for a previous folder).
void refresh_tab_entries(AppState& app, Tab& tab, const std::string& watched,
                         const std::unordered_set<std::string>& names) {
  if (watched.empty() || tab.current_path != watched) return;
  for (auto& e : tab.entries) {
    if (names.count(e.name)) refresh_entry_from_disk(e);
  }
}

}  // namespace

int dir_watch_init() { return ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC); }

bool dir_watch_sync(AppState& app) {
  if (app.dir_watch_fd < 0) return false;

  const std::string tab_path =
      app.tabs[app.active_tab].current_path == "computer://"
          ? std::string{}
          : app.tabs[app.active_tab].current_path;
  arm_watch(app, tab_path, app.dir_watch_tab_wd, app.dir_watch_tab_path);

  const std::string pane_path =
      app.split_view && app.right_pane.current_path != "computer://"
          ? app.right_pane.current_path
          : std::string{};
  arm_watch(app, pane_path, app.dir_watch_pane_wd, app.dir_watch_pane_path);

  return app.dir_watch_tab_wd >= 0;
}

void dir_watch_process(AppState& app, bool fresh, bool& need_reload_tab,
                       bool& need_reload_pane) {
  need_reload_tab = false;
  need_reload_pane = false;
  if (app.dir_watch_fd < 0 || !fresh) return;

  std::array<char, 65536> buf;
  std::unordered_set<std::string> refresh_tab, refresh_pane;
  bool overflow = false;

  for (;;) {
    ssize_t n = ::read(app.dir_watch_fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      break;  // EAGAIN: drained
    }
    if (n == 0) break;

    size_t off = 0;
    while (off + sizeof(struct inotify_event) <= static_cast<size_t>(n)) {
      const auto* ev =
          reinterpret_cast<const struct inotify_event*>(buf.data() + off);
      if (off + sizeof(struct inotify_event) + ev->len >
          static_cast<size_t>(n))
        break;  // trailing partial record: aligns on the next read
      off += sizeof(struct inotify_event) + ev->len;

      if (ev->mask & IN_Q_OVERFLOW) {
        overflow = true;
        continue;
      }

      const bool is_tab = (ev->wd == app.dir_watch_tab_wd);
      const bool is_pane = (ev->wd == app.dir_watch_pane_wd);
      if (!is_tab && !is_pane) continue;  // removed/recreated watch

      // The watched folder itself disappeared or the watch was dropped:
      // clear it so the next dir_watch_sync re-arms (or navigates home).
      if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        if (is_tab) need_reload_tab = true;
        if (is_pane) need_reload_pane = true;
      }
      if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
        if (is_tab) app.dir_watch_tab_wd = -1;
        if (is_pane) app.dir_watch_pane_wd = -1;
        continue;
      }

      if (ev->len == 0) continue;  // event on the directory itself

      std::string name(ev->name, static_cast<size_t>(ev->len));
      const auto nul = name.find('\0');
      if (nul != std::string::npos) name.erase(nul);

      if (ev->mask & kStructuralMask) {
        if (is_tab) need_reload_tab = true;
        if (is_pane) need_reload_pane = true;
      } else if (ev->mask & (IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE)) {
        if (is_tab) refresh_tab.insert(name);
        if (is_pane) refresh_pane.insert(std::move(name));
      }
    }
  }

  if (overflow) {
    need_reload_tab = true;
    need_reload_pane = true;
  }

  if (!refresh_tab.empty() || !refresh_pane.empty()) {
    std::lock_guard<std::mutex> lk(app.dir_watch_mtx);
    for (auto& n : refresh_tab) app.dir_watch_refresh_names.insert(std::move(n));
    for (auto& n : refresh_pane)
      app.dir_watch_pane_refresh_names.insert(std::move(n));
  }
}

bool dir_watch_refresh_entries(AppState& app) {
  if (app.dir_watch_fd < 0) return false;

  std::unordered_set<std::string> tab_names, pane_names;
  {
    std::lock_guard<std::mutex> lk(app.dir_watch_mtx);
    tab_names.swap(app.dir_watch_refresh_names);
    pane_names.swap(app.dir_watch_pane_refresh_names);
  }

  bool changed = false;
  if (!tab_names.empty()) {
    refresh_tab_entries(app, app.tabs[app.active_tab],
                        app.dir_watch_tab_path, tab_names);
    changed = true;
  }
  if (!pane_names.empty() && app.split_view) {
    refresh_tab_entries(app, app.right_pane, app.dir_watch_pane_path,
                        pane_names);
    changed = true;
  }
  return changed;
}

void dir_watch_close(AppState& app) {
  if (app.dir_watch_fd >= 0) {
    ::close(app.dir_watch_fd);
    app.dir_watch_fd = -1;
  }
  app.dir_watch_tab_wd = -1;
  app.dir_watch_pane_wd = -1;
  app.dir_watch_tab_path.clear();
  app.dir_watch_pane_path.clear();
  {
    std::lock_guard<std::mutex> lk(app.dir_watch_mtx);
    app.dir_watch_refresh_names.clear();
    app.dir_watch_pane_refresh_names.clear();
  }
}

}  // namespace eh::file_browser