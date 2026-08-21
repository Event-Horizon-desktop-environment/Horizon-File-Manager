#include "app/file_browser/features/tab_history.hpp"

#include "../app.hpp"

#include <algorithm>

namespace eh::file_browser {

namespace {
constexpr size_t kMaxClosedTabs = 10;
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

} // namespace eh::file_browser
