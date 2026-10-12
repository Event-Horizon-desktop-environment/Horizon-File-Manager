// filemanager1.cpp — see filemanager1.hpp.
#include "app/file_browser/features/filemanager1/filemanager1.hpp"

#include "app/file_browser/features/launch/launch_path.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <map>
#include <thread>

#include <unistd.h>
#include <sys/types.h>

namespace fs = std::filesystem;

namespace eh::file_browser {

namespace {

constexpr const char* kBusName = "org.freedesktop.FileManager1";
constexpr const char* kObjectPath = "/org/freedesktop/FileManager1";
constexpr const char* kInterface = "org.freedesktop.FileManager1";

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += '\'';
  return out;
}

void spawn_detached(const std::string& path) {
  if (path.empty()) return;
  // Fork+exec a new browser window. SIGCHLD is set to SIG_IGN by the service
  // entry points so children are auto-reaped and never become zombies.
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid == 0) {
    setsid();
    execlp("horizon-files", "horizon-files", path.c_str(), nullptr);
    // Fallback when exec fails (e.g. dev builds without install): try the
    // absolute bindir via PATH lookup already done by execlp, then give up.
    _exit(1);
  }
}

void handle_uris(const std::vector<std::string>& uris, bool select_files) {
  // Group files by parent dir: our CLI reveals a single file per window, so
  // multiple files in one folder open one window selecting the first.
  std::map<std::string, std::string> dir_first_file;
  std::vector<std::string> dirs;
  for (const auto& uri : uris) {
    std::string p = normalize_launch_path(uri);
    if (p.empty()) continue;
    // Plain paths from older callers may be relative; the window's
    // navigate_to() absolutizes, but grouping needs a stable key.
    std::string norm = fs::path(p).lexically_normal().string();
    if (norm.empty()) continue;
    std::error_code ec;
    const bool is_dir = fs::is_directory(norm, ec) && !ec;
    if (!select_files || is_dir) {
      if (dir_first_file.find(norm) == dir_first_file.end() &&
          std::find(dirs.begin(), dirs.end(), norm) == dirs.end())
        dirs.push_back(norm);
    } else {
      fs::path fp(norm);
      std::string parent = fp.has_parent_path()
                               ? fp.parent_path().lexically_normal().string()
                               : std::string("/");
      if (parent.empty()) parent = "/";
      if (dir_first_file.find(parent) == dir_first_file.end())
        dir_first_file[parent] = norm;
    }
  }
  for (const auto& d : dirs) spawn_detached(d);
  for (const auto& [dir, file] : dir_first_file) {
    (void)dir;
    spawn_detached(file);
  }
  // Shell fallback is unnecessary: fork+exec above never uses a shell, so no
  // quoting concerns. shell_quote kept for future system() callers.
  (void)shell_quote;
}

}  // namespace

std::string filemanager1_uri_to_path(const std::string& uri) {
  return normalize_launch_path(uri);
}

void filemanager1_open_path(const std::string& path) {
  try {
    spawn_detached(path);
  } catch (...) {
  }
}

void filemanager1_run_service() {
  // Auto-reap spawned windows.
  std::signal(SIGCHLD, SIG_IGN);
  std::unique_ptr<sdbus::IConnection> conn;
  try {
    conn = sdbus::createSessionBusConnection();
  } catch (...) {
    return;  // no session bus (headless/CI): nothing to own
  }
  try {
    conn->requestName(sdbus::ServiceName{kBusName});
  } catch (...) {
    return;  // already owned (another Horizon instance serves): nothing to do
  }
  std::unique_ptr<sdbus::IObject> obj;
  try {
    obj = sdbus::createObject(*conn, sdbus::ObjectPath{kObjectPath});
    obj->addVTable(
            sdbus::registerMethod("ShowFolders")
                .withInputParamNames("URIs", "startup_id")
                .withOutputParamNames()
                .implementedAs(
                    [](const std::vector<std::string>& uris,
                       const std::string&) { handle_uris(uris, false); }),
            sdbus::registerMethod("ShowItems")
                .withInputParamNames("URIs", "startup_id")
                .withOutputParamNames()
                .implementedAs(
                    [](const std::vector<std::string>& uris,
                       const std::string&) { handle_uris(uris, true); }),
            // No dedicated properties dialog: revealing the file is strictly
            // more useful than an error reply.
            sdbus::registerMethod("ShowItemProperties")
                .withInputParamNames("URIs", "startup_id")
                .withOutputParamNames()
                .implementedAs(
                    [](const std::vector<std::string>& uris,
                       const std::string&) { handle_uris(uris, true); }))
        .forInterface(sdbus::InterfaceName{kInterface});
  } catch (...) {
    return;
  }
  try {
    conn->enterEventLoop();
  } catch (...) {
  }
}

void filemanager1_start_in_background() {
  try {
    std::thread([] { filemanager1_run_service(); }).detach();
  } catch (...) {
  }
}

}  // namespace eh::file_browser
