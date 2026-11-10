#include "app/file_browser/features/appimage/appimage.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace eh::file_browser {
namespace {

std::mutex g_mtx;
std::unordered_map<std::string, std::string> g_icons;  // path -> cached icon
std::unordered_set<std::string> g_inflight;

bool has_appimage_suffix(const std::string& path) {
  const std::string ext = fs::path(path).extension().string();
  if (ext.size() != 9) return false;
  std::string lower;
  lower.reserve(ext.size());
  for (unsigned char c : ext)
    lower.push_back(static_cast<char>(std::tolower(c)));
  return lower == ".appimage";
}

std::string cache_root() {
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/horizon-files/appimage-icons";
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  return std::string(home) + "/.cache/horizon-files/appimage-icons";
}

std::string cache_key(const std::string& path) {
  std::error_code ec;
  const fs::path abs = fs::absolute(path, ec);
  const std::string canonical = ec ? path : abs.lexically_normal().string();
  std::uint64_t h = std::hash<std::string>{}(canonical);

  struct stat st {};
  if (::stat(canonical.c_str(), &st) == 0) {
    h ^= static_cast<std::uint64_t>(st.st_size) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(st.st_mtime) + 0x9e3779b97f4a7c15ULL +
         (h << 6) + (h >> 2);
  }

  std::string stem = fs::path(canonical).stem().string();
  if (stem.empty()) stem = "appimage";
  for (char& c : stem) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
      c = '_';
  }
  if (stem.size() > 48) stem.resize(48);

  char hex[32];
  std::snprintf(hex, sizeof(hex), "%016llx",
                static_cast<unsigned long long>(h));
  return stem + "-" + hex;
}

bool ensure_executable(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  if ((st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) return true;
  return ::chmod(path.c_str(), st.st_mode | S_IRWXU) == 0;
}

std::string sniff_icon_ext(const std::string& file) {
  int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return {};
  unsigned char buf[512]{};
  ssize_t n = ::read(fd, buf, sizeof(buf));
  ::close(fd);
  if (n <= 0) return {};
  if (n >= 8 && std::memcmp(buf, "\x89PNG\r\n\x1a\n", 8) == 0) return ".png";
  if (n >= 4 && buf[0] == 0xff && buf[1] == 0xd8) return ".jpg";
  std::string head(reinterpret_cast<char*>(buf),
                   static_cast<std::size_t>(std::min<ssize_t>(n, 256)));
  for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (head.find("<svg") != std::string::npos) return ".svg";
  return {};
}

std::string cached_icon_in(const fs::path& dir) {
  for (const char* name : {"icon.png", "icon.svg", "icon.jpg", ".DirIcon"}) {
    std::error_code ec;
    fs::path p = dir / name;
    if (fs::is_regular_file(p, ec) && !ec) return p.string();
  }
  return {};
}

// Copy an extracted icon into the stable cache directory so the temporary
// squashfs root can be deleted.
std::string install_icon(const fs::path& src, const fs::path& final_dir) {
  std::error_code ec;
  fs::path canon = fs::canonical(src, ec);
  if (ec) canon = src;
  std::string ext = sniff_icon_ext(canon.string());
  if (ext.empty()) {
    std::string e = canon.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e == ".png" || e == ".svg" || e == ".jpg") ext = e;
  }
  if (ext.empty()) return {};

  fs::create_directories(final_dir, ec);
  if (ec) return {};
  fs::path dest = final_dir / ("icon" + ext);
  ec.clear();
  fs::copy_file(canon, dest, fs::copy_options::overwrite_existing, ec);
  if (ec) return {};
  return dest.string();
}

std::string find_icon_in_tree(const fs::path& root) {
  std::error_code ec;
  // The AppImage convention: .DirIcon at the AppDir root (often a symlink).
  fs::path dir_icon = root / ".DirIcon";
  if (fs::is_regular_file(dir_icon, ec) && !ec) return dir_icon.string();
  ec.clear();
  if (fs::is_symlink(dir_icon, ec) && !ec) {
    fs::path resolved = fs::canonical(dir_icon, ec);
    if (!ec && fs::is_regular_file(resolved)) return resolved.string();
  }

  // Fall back to the first usable root-level image.
  for (const char* ext : {".png", ".svg", ".jpg"}) {
    ec.clear();
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      std::error_code fec;
      if (!it->is_regular_file(fec) || fec) continue;
      std::string e = it->path().extension().string();
      for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (e == ext) return it->path().string();
    }
  }

  // Last resort: bounded recursive scan (these trees only contain the
  // patterns we explicitly extracted, so it cannot walk a full AppImage).
  int budget = 2048;
  ec.clear();
  for (fs::recursive_directory_iterator
           it(root, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end && budget-- > 0; it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec) || fec) continue;
    std::string e = it->path().extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e == ".png" || e == ".svg" || e == ".jpg") return it->path().string();
  }
  return {};
}

std::string desktop_icon_field(const fs::path& desktop_file) {
  std::ifstream in(desktop_file);
  if (!in) return {};
  std::string line;
  bool in_entry = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "[Desktop Entry]") { in_entry = true; continue; }
    if (!line.empty() && line[0] == '[') { if (in_entry) break; continue; }
    if (!in_entry || line.rfind("Icon=", 0) != 0) continue;
    std::string icon = line.substr(5);
    while (!icon.empty() && (icon.back() == ' ' || icon.back() == '\t'))
      icon.pop_back();
    return icon;
  }
  return {};
}

// Run `<appimage> --appimage-extract <pattern>` with cwd=dir. Returns true
// when the runtime exited successfully (or at least produced files).
bool run_extract(const std::string& appimage, const std::string& dir,
                 const std::string& pattern);

// The AppDir convention is a `.DirIcon` symlink (often to an icon-theme path
// such as usr/share/icons/...). Extracting `.DirIcon` alone leaves a dangling
// symlink because the runtime does not follow it. Resolve the chain and pull
// the real file out, staying inside the extracted AppDir.
std::string follow_and_extract_symlink(const std::string& appimage,
                                       const fs::path& squash_root,
                                       fs::path candidate) {
  const fs::path root = squash_root.lexically_normal();
  const std::string root_s = root.string();
  for (int depth = 0; depth < 6; ++depth) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec) && !ec) return candidate.string();
    if (!fs::is_symlink(candidate, ec) || ec) break;

    fs::path target = fs::read_symlink(candidate, ec);
    if (ec) break;

    fs::path next;
    if (target.is_absolute()) {
      // Build-time absolute paths are meaningless on the host: treat the
      // AppDir-root-relative part as the extracted path.
      next = root / target.relative_path();
    } else {
      next = candidate.parent_path() / target;
    }
    next = next.lexically_normal();
    const std::string next_s = next.string();
    if (next_s.compare(0, root_s.size(), root_s) != 0) break;

    std::string rel = fs::relative(next, root, ec).string();
    if (ec || rel.empty() || rel == ".") break;
    run_extract(appimage, root.parent_path().string(), rel);
    candidate = next;
  }
  std::error_code ec;
  if (fs::is_regular_file(candidate, ec) && !ec) return candidate.string();
  return {};
}

bool run_extract(const std::string& appimage, const std::string& dir,
                 const std::string& pattern) {
  pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    ::setsid();
    if (::chdir(dir.c_str()) != 0) _exit(127);
    ::execl(appimage.c_str(), appimage.c_str(), "--appimage-extract",
            pattern.c_str(), nullptr);
    _exit(127);
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(8);
  int status = 0;
  for (;;) {
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) break;
    if (r < 0) return false;
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string extract_icon_locked(const std::string& path) {
  const std::string root = cache_root();
  if (root.empty()) return {};
  if (!ensure_executable(path)) return {};

  const std::string key = cache_key(path);
  fs::path final_dir = fs::path(root) / key;
  std::error_code ec;
  std::string existing = cached_icon_in(final_dir);
  if (!existing.empty()) return existing;

  const std::string tmp =
      (final_dir.string() + ".tmp." + std::to_string(::getpid()));
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  if (ec) return {};

  const fs::path squash = fs::path(tmp) / "squashfs-root";
  fs::path found;

  // 1. The standard AppImage icon.
  run_extract(path, tmp, ".DirIcon");
  if (fs::exists(squash, ec) && !ec) {
    fs::path dir_icon = squash / ".DirIcon";
    ec.clear();
    if (fs::exists(dir_icon, ec) || fs::is_symlink(dir_icon, ec)) {
      std::string resolved = follow_and_extract_symlink(path, squash, dir_icon);
      if (!resolved.empty()) found = resolved;
    }
    if (found.empty()) {
      std::string candidate = find_icon_in_tree(squash);
      if (!candidate.empty()) found = candidate;
    }
  }

  // 2. No .DirIcon: read the bundled .desktop file's Icon= entry, which may
  //    point at another file inside the AppDir.
  if (found.empty()) {
    ec.clear();
    run_extract(path, tmp, "*.desktop");
    std::string icon_rel;
    ec.clear();
    if (fs::exists(squash, ec) && !ec) {
      for (fs::directory_iterator
               it(squash, fs::directory_options::skip_permission_denied, ec),
           end;
           !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        if (it->path().extension() != ".desktop") continue;
        icon_rel = desktop_icon_field(it->path());
        if (!icon_rel.empty()) break;
      }
    }
    if (!icon_rel.empty()) {
      if (icon_rel[0] == '/') {
        if (fs::is_regular_file(icon_rel, ec) && !ec) found = icon_rel;
      } else {
        fs::path direct = squash / icon_rel;
        if (fs::is_regular_file(direct, ec) && !ec) {
          found = direct;
        } else {
          for (const char* ext : {"", ".png", ".svg"}) {
            fs::path p = squash / (icon_rel + ext);
            ec.clear();
            if (fs::is_regular_file(p, ec) && !ec) {
              found = p;
              break;
            }
          }
        }
      }
    }
  }

  // 3. Fall back to root-level image files.
  if (found.empty()) {
    run_extract(path, tmp, "*.png");
    run_extract(path, tmp, "*.svg");
    ec.clear();
    if (fs::exists(squash, ec) && !ec) {
      std::string candidate = find_icon_in_tree(squash);
      if (!candidate.empty()) found = candidate;
    }
  }

  std::string result;
  if (!found.empty()) result = install_icon(found, final_dir);
  fs::remove_all(tmp, ec);
  return result;
}

}  // namespace

bool is_appimage_path(const std::string& path) {
  return has_appimage_suffix(path);
}

std::string appimage_icon_cached(const std::string& path) {
  if (!has_appimage_suffix(path)) return {};
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_icons.find(path);
    if (it != g_icons.end()) return it->second;
  }
  const std::string root = cache_root();
  if (root.empty()) return {};
  std::error_code ec;
  std::string icon = cached_icon_in(fs::path(root) / cache_key(path));
  if (!icon.empty()) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_icons[path] = icon;
  }
  return icon;
}

std::string appimage_icon_path(const std::string& path) {
  if (!has_appimage_suffix(path)) return {};
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_icons.find(path);
    if (it != g_icons.end()) return it->second;
    if (g_inflight.count(path)) return {};
    g_inflight.insert(path);
  }

  std::string icon = extract_icon_locked(path);

  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_inflight.erase(path);
    if (!icon.empty()) g_icons[path] = icon;
  }
  return icon;
}

bool launch_appimage(const std::string& path) {
  if (path.empty() || !ensure_executable(path)) return false;

  // AppImages normally mount through FUSE. On systems without /dev/fuse the
  // runtime's own extract-and-run mode keeps them working.
  struct stat fuse_st {};
  const bool have_fuse = ::stat("/dev/fuse", &fuse_st) == 0;

  pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    ::setsid();
    if (!have_fuse) ::setenv("APPIMAGE_EXTRACT_AND_RUN", "1", 1);
    ::execl(path.c_str(), path.c_str(), nullptr);
    _exit(127);
  }
  return true;
}

bool launch_program(const std::string& path) {
  if (is_appimage_path(path)) return launch_appimage(path);
  if (path.empty() || !ensure_executable(path)) return false;

  pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    ::setsid();
    ::execl(path.c_str(), path.c_str(), nullptr);
    _exit(127);
  }
  return true;
}

}  // namespace eh::file_browser
