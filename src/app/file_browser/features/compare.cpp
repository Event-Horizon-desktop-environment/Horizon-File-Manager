#include "app/file_browser/features/compare.hpp"

#include "../app.hpp"

#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace eh::file_browser {

namespace {

constexpr std::array<const char*, 3> kTools{"meld", "kompare", "diffuse"};

std::uint64_t toast_expiry_3s() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             (std::chrono::steady_clock::now() + std::chrono::milliseconds(3000))
                 .time_since_epoch())
      .count();
}

bool executable_on_path(const char* name) {
  const char* path_env = std::getenv("PATH");
  if (!path_env || !*path_env) return false;
  std::string paths(path_env);
  size_t start = 0;
  while (start <= paths.size()) {
    size_t end = paths.find(':', start);
    std::string dir = end == std::string::npos
                          ? paths.substr(start)
                          : paths.substr(start, end - start);
    if (!dir.empty() && dir.back() != '/') dir += '/';
    dir += name;
    if (::access(dir.c_str(), X_OK) == 0) return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

} // namespace

bool compare_tool_available() {
  for (const char* t : kTools)
    if (executable_on_path(t)) return true;
  return false;
}

const char* compare_tool_name() {
  for (const char* t : kTools)
    if (executable_on_path(t)) return t;
  return "";
}

void compare_selected_files(AppState& app) {
  auto& tab = app.cur_tab();

  // Collect selected paths (multi-selection, falling back to single selection)
  std::vector<std::string> sel_paths;
  for (int vis_idx : tab.multi_selected) {
    if (vis_idx < 0 || vis_idx >= static_cast<int>(tab.visible_entries.size())) continue;
    int r = tab.visible_entries[vis_idx];
    if (r >= 0 && r < static_cast<int>(tab.entries.size()))
      sel_paths.push_back(tab.entries[r].path);
  }

  const char* tool = compare_tool_name();
  if (sel_paths.size() != 2) {
    app.operation_status = "Select exactly two items to compare";
    app.operation_status_expires_ms = toast_expiry_3s();
    app.pendingRedraw = true;
    return;
  }
  if (!*tool) {
    app.operation_status = "No diff tool found (install meld, kompare or diffuse)";
    app.operation_status_expires_ms = toast_expiry_3s();
    app.pendingRedraw = true;
    return;
  }

  pid_t pid = ::fork();
  if (pid == 0) {
    execlp(tool, tool, sel_paths[0].c_str(), sel_paths[1].c_str(),
           static_cast<char*>(nullptr));
    _exit(127);
  }

  app.operation_status = std::string("Comparing with ") + tool;
  app.operation_status_expires_ms = toast_expiry_3s();
  app.pendingRedraw = true;
}

} // namespace eh::file_browser
