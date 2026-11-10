// hyprland_position.cpp — query Hyprland's IPC for the file manager window's
// screen position. Layer-shell popups are output-anchored, so unlike
// subsurfaces they need an absolute origin instead of a parent-relative one.

#include "app/file_browser/features/popup_position/hyprland_position.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace eh::file_browser {

namespace {

std::string hypr_request(const char* request) {
  const char* sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (!sig || !*sig) return {};

  std::vector<std::string> paths;
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
    paths.push_back(std::string(runtime) + "/hypr/" + sig + "/.socket.sock");
  paths.push_back(std::string("/tmp/hypr/") + sig + "/.socket.sock");

  for (const std::string& path : paths) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) continue;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      ::close(fd);
      continue;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      continue;
    }

    const std::size_t len = std::strlen(request);
    if (::write(fd, request, len) != static_cast<ssize_t>(len)) {
      ::close(fd);
      continue;
    }

    std::string out;
    char buf[8192];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, n);
    ::close(fd);
    return out;
  }
  return {};
}

// Split a JSON array into its top-level object substrings.
std::vector<std::string> top_level_objects(const std::string& json) {
  std::vector<std::string> out;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  std::size_t start = 0;
  for (std::size_t i = 0; i < json.size(); ++i) {
    const char c = json[i];
    if (in_string) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      if (depth == 0) start = i;
      ++depth;
    } else if (c == '}') {
      if (depth > 0) --depth;
      if (depth == 0) out.push_back(json.substr(start, i - start + 1));
    }
  }
  return out;
}

bool json_number(const std::string& obj, const char* key, double& out) {
  const std::string pat = std::string("\"") + key + "\"";
  const std::size_t p = obj.find(pat);
  if (p == std::string::npos) return false;
  const std::size_t colon = obj.find(':', p + pat.size());
  if (colon == std::string::npos) return false;
  const char* begin = obj.c_str() + colon + 1;
  while (*begin == ' ' || *begin == '\t' || *begin == '\n' || *begin == '\r')
    ++begin;
  char* end = nullptr;
  const double v = std::strtod(begin, &end);
  if (end == begin) return false;
  out = v;
  return true;
}

bool json_pair(const std::string& obj, const char* key, int& a, int& b) {
  const std::string pat = std::string("\"") + key + "\"";
  const std::size_t p = obj.find(pat);
  if (p == std::string::npos) return false;
  const std::size_t lb = obj.find('[', p + pat.size());
  const std::size_t comma = obj.find(',', lb);
  const std::size_t rb = obj.find(']', comma);
  if (lb == std::string::npos || comma == std::string::npos ||
      rb == std::string::npos)
    return false;
  a = static_cast<int>(std::lround(std::strtod(obj.c_str() + lb + 1, nullptr)));
  b = static_cast<int>(std::lround(std::strtod(obj.c_str() + comma + 1, nullptr)));
  return true;
}

bool parse_rect(const std::string& obj, WindowRect& r) {
  double x = 0, y = 0, w = 0, h = 0;
  if (!json_number(obj, "x", x) || !json_number(obj, "y", y) ||
      !json_number(obj, "width", w) || !json_number(obj, "height", h))
    return false;
  r.x = static_cast<int>(std::lround(x));
  r.y = static_cast<int>(std::lround(y));
  r.w = static_cast<int>(std::lround(w));
  r.h = static_cast<int>(std::lround(h));
  return r.w > 0 && r.h > 0;
}

}  // namespace

bool hyprland_window_rect(WindowRect& out) {
  const std::vector<std::string> clients =
      top_level_objects(hypr_request("j/clients"));
  const long pid = static_cast<long>(::getpid());

  for (const std::string& client : clients) {
    double p = 0;
    if (!json_number(client, "pid", p) ||
        static_cast<long>(std::lround(p)) != pid)
      continue;
    if (json_pair(client, "at", out.x, out.y) &&
        json_pair(client, "size", out.w, out.h))
      return true;
  }
  return false;
}

bool hyprland_monitor_for_point(int gx, int gy, WindowRect& out) {
  const std::vector<std::string> monitors =
      top_level_objects(hypr_request("j/monitors"));

  WindowRect first{};
  bool have_first = false;
  for (const std::string& monitor : monitors) {
    WindowRect r{};
    if (!parse_rect(monitor, r)) continue;
    if (!have_first) {
      first = r;
      have_first = true;
    }
    if (gx >= r.x && gx < r.x + r.w && gy >= r.y && gy < r.y + r.h) {
      out = r;
      return true;
    }
  }
  if (have_first) {
    out = first;
    return true;
  }
  return false;
}

}  // namespace eh::file_browser
