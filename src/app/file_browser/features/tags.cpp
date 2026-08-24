#include "app/file_browser/features/tags.hpp"

#include <sys/xattr.h>

#include <cerrno>
#include <cstddef>
#include <vector>

namespace eh::file_browser {
namespace {

constexpr const char kTagXattr[] = "user.xdg.tags";

std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

} // namespace

std::string read_xdg_tags(const std::string& path) {
  const ssize_t need = ::getxattr(path.c_str(), kTagXattr, nullptr, 0);
  if (need <= 0) return {};
  std::vector<char> buf(static_cast<std::size_t>(need));
  const ssize_t got =
      ::getxattr(path.c_str(), kTagXattr, buf.data(), buf.size());
  if (got <= 0) return {};
  return trim(std::string(buf.data(), static_cast<std::size_t>(got)));
}

bool write_xdg_tags(const std::string& path, const std::string& tags) {
  const std::string val = trim(tags);
  if (val.empty()) {
    // No tags left — drop the attribute entirely.
    return ::removexattr(path.c_str(), kTagXattr) == 0 || errno == ENODATA;
  }
  return ::setxattr(path.c_str(), kTagXattr, val.c_str(), val.size(), 0) == 0;
}

} // namespace eh::file_browser
