#include "app/file_browser/features/tab_history/tab_history.hpp"

#include "../../app.hpp"

#include "config/shell_config.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <sys/stat.h>

#include <toml++/toml.hpp>

namespace eh::file_browser {

namespace {
constexpr size_t kMaxClosedTabs = 10;
constexpr size_t kMaxSessionTabs = 32;

// Local absolute dir only: remote URIs need live connections (hang risk
// at startup) and virtual rows aren't locations.
bool session_path_ok(const std::string& p) {
  if (p.empty() || p[0] != '/') return false;
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t steady_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

void remember_closed_tab(AppState& app, const Tab& tab) {
  AppState::ClosedTab rec;
  rec.path = tab.current_path;
  rec.view_mode = tab.view_mode;
  app.closed_tabs.insert(app.closed_tabs.begin(), std::move(rec));
  if (app.closed_tabs.size() > kMaxClosedTabs)
    app.closed_tabs.resize(kMaxClosedTabs);
}

bool reopen_last_closed_tab(AppState& app) {
  if (app.closed_tabs.empty()) return false;

  AppState::ClosedTab rec = std::move(app.closed_tabs.front());
  app.closed_tabs.erase(app.closed_tabs.begin());

  Tab t;
  t.current_path = rec.path;
  t.view_mode = rec.view_mode;
  app.tabs.push_back(std::move(t));
  app.active_tab = static_cast<int>(app.tabs.size()) - 1;
  navigate_to(app, app.cur_tab().current_path);
  reload_dir(app);
  return true;
}

std::string session_snapshot(AppState& app) {
  toml::table tbl;
  std::vector<std::string> paths;
  for (auto& t : app.tabs) {
    if (session_path_ok(t.current_path) &&
        paths.size() < kMaxSessionTabs)
      paths.push_back(t.current_path);
  }
  if (paths.empty()) return {};
  // Restore by path (not index): unrecoverable entries are dropped, so a
  // stored index could point at the wrong tab.
  std::string active_path;
  if (!app.tabs.empty()) {
    int ai = std::clamp(app.active_tab, 0,
                        static_cast<int>(app.tabs.size()) - 1);
    if (session_path_ok(app.tabs[static_cast<size_t>(ai)].current_path))
      active_path = app.tabs[static_cast<size_t>(ai)].current_path;
  }
  if (active_path.empty()) active_path = paths.front();
  tbl.emplace("active_path", active_path);
  tbl.emplace("split", app.split_view);
  if (app.split_view && session_path_ok(app.right_pane.current_path))
    tbl.emplace("right_path", app.right_pane.current_path);
  toml::array arr;
  for (auto& p : paths) arr.push_back(p);
  tbl.emplace("tabs", std::move(arr));
  std::ostringstream oss;
  oss << tbl;
  return oss.str();
}

void session_write(AppState& app) {
  std::string text = session_snapshot(app);
  app.session_last_saved = text;
  app.session_last_ms = steady_ms_now();
  if (text.empty()) return;
  std::string path = eh::config::session_toml_path();
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path(path).parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return;
  out << text;
}

void session_save_maybe(AppState& app) {
  int64_t now = steady_ms_now();
  if (now - app.session_last_ms < 5000) return;
  app.session_last_ms = now;
  std::string text = session_snapshot(app);
  if (text.empty() || text == app.session_last_saved) return;
  session_write(app);
  // session_write stamps last_ms/last_saved; keep the throttle base.
  app.session_last_ms = now;
}

bool session_restore(AppState& app) {
  std::ifstream f(eh::config::session_toml_path());
  if (!f.is_open()) return false;
  toml::table tbl;
  try {
    tbl = toml::parse(f);
  } catch (...) {
    return false;
  }
  std::vector<std::string> paths;
  if (auto* arr = tbl["tabs"].as_array()) {
    for (auto& v : *arr) {
      if (auto* s = v.as_string()) {
        if (session_path_ok(s->get()) && paths.size() < kMaxSessionTabs)
          paths.push_back(s->get());
      }
    }
  }
  if (paths.empty()) return false;
  std::string active_path;
  if (auto* s = tbl["active_path"].as_string()) active_path = s->get();
  bool split = tbl["split"].value_or(false);
  std::string right_path;
  if (auto* s = tbl["right_path"].as_string()) right_path = s->get();
  app.tabs.clear();
  for (auto& p : paths) {
    Tab t;
    t.current_path = p;
    app.tabs.push_back(std::move(t));
  }
  app.active_tab = 0;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (paths[i] == active_path) {
      app.active_tab = static_cast<int>(i);
      break;
    }
  }
  app.split_view = split && session_path_ok(right_path);
  if (app.split_view) app.right_pane.current_path = right_path;
  navigate_to(app, app.cur_tab().current_path);
  reload_dir(app);
  app.session_last_saved = session_snapshot(app);
  app.session_last_ms = steady_ms_now();
  return true;
}

} // namespace eh::file_browser
