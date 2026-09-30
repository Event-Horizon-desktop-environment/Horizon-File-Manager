#include "app/file_browser/features/tags/tags.hpp"

#include <sys/xattr.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace eh::file_browser {
namespace {

constexpr const char kTagXattr[] = "user.xdg.tags";
constexpr const char kRatingXattr[] = "user.xdg.rating";
constexpr const char kCommentXattr[] = "user.xdg.comment";
constexpr std::size_t kCommentMax = 1024;

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

int read_xdg_rating(const std::string& path) {
  char buf[16]{};
  const ssize_t got = ::getxattr(path.c_str(), kRatingXattr, buf, sizeof(buf) - 1);
  if (got <= 0) return 0;
  int v = 0;
  for (ssize_t i = 0; i < got; ++i) {
    if (buf[i] >= '0' && buf[i] <= '9') v = v * 10 + (buf[i] - '0');
    else break;
  }
  return std::clamp(v, 0, 5);
}

bool write_xdg_rating(const std::string& path, int rating) {
  rating = std::clamp(rating, 0, 5);
  if (rating == 0)
    return ::removexattr(path.c_str(), kRatingXattr) == 0 || errno == ENODATA;
  char v = static_cast<char>('0' + rating);
  return ::setxattr(path.c_str(), kRatingXattr, &v, 1, 0) == 0;
}

std::string read_xdg_comment(const std::string& path) {
  const ssize_t need = ::getxattr(path.c_str(), kCommentXattr, nullptr, 0);
  if (need <= 0) return {};
  std::vector<char> buf(static_cast<std::size_t>(std::min<ssize_t>(need, 4096)));
  const ssize_t got =
      ::getxattr(path.c_str(), kCommentXattr, buf.data(), buf.size());
  if (got <= 0) return {};
  return trim(std::string(buf.data(), static_cast<std::size_t>(got)));
}

bool write_xdg_comment(const std::string& path, const std::string& comment) {
  std::string val = trim(comment);
  if (val.size() > kCommentMax) val.resize(kCommentMax);
  if (val.empty())
    return ::removexattr(path.c_str(), kCommentXattr) == 0 || errno == ENODATA;
  return ::setxattr(path.c_str(), kCommentXattr, val.c_str(), val.size(), 0) == 0;
}


std::vector<TagDef> default_tags() {
  return {
      {"Important", 0xe5 / 255.0, 0x48 / 255.0, 0x4d / 255.0},
      {"Work", 0x3e / 255.0, 0x8e / 255.0, 0xf7 / 255.0},
      {"Personal", 0x46 / 255.0, 0xa7 / 255.0, 0x58 / 255.0},
      {"Later", 0xf5 / 255.0, 0xa5 / 255.0, 0x24 / 255.0},
      {"Project", 0x8e / 255.0, 0x4e / 255.0, 0xc6 / 255.0},
      {"Review", 0xe9 / 255.0, 0x3d / 255.0, 0x82 / 255.0},
      {"Archive", 0x8b / 255.0, 0x8d / 255.0, 0x98 / 255.0},
  };
}

std::vector<TagDef> tag_registry(
    const std::map<std::string, std::string>& color_overrides) {
  auto tags = default_tags();
  for (auto& t : tags) {
    auto it = color_overrides.find(t.name);
    if (it == color_overrides.end()) continue;
    double r, g, b;
    if (parse_hex_color(it->second, r, g, b)) {
      t.r = r;
      t.g = g;
      t.b = b;
    }
  }
  return tags;
}

bool parse_hex_color(const std::string& hex, double& r, double& g,
                     double& b) {
  std::string h = trim(hex);
  if (!h.empty() && h[0] == '#') h.erase(h.begin());
  if (h.size() != 6 && h.size() != 3) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  auto byte = [&](size_t i) -> int {
    if (h.size() == 6) {
      int hi = nib(h[i * 2]), lo = nib(h[i * 2 + 1]);
      if (hi < 0 || lo < 0) return -1;
      return hi * 16 + lo;
    }
    int v = nib(h[i]);
    if (v < 0) return -1;
    return v * 17;
  };
  int rv = byte(0), gv = byte(1), bv = byte(2);
  if (rv < 0 || gv < 0 || bv < 0) return false;
  r = rv / 255.0;
  g = gv / 255.0;
  b = bv / 255.0;
  return true;
}

std::string hex_color(double r, double g, double b) {
  auto clamp8 = [](double v) {
    int i = static_cast<int>(v * 255.0 + 0.5);
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    return i;
  };
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", clamp8(r), clamp8(g),
           clamp8(b));
  return buf;
}

std::vector<std::string> split_tags(const std::string& csv) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= csv.size()) {
    size_t j = csv.find(',', i);
    if (j == std::string::npos) j = csv.size();
    std::string t = trim(csv.substr(i, j - i));
    if (!t.empty()) out.push_back(t);
    if (j == csv.size()) break;
    i = j + 1;
  }
  return out;
}

std::string join_tags(const std::vector<std::string>& tags) {
  std::string out;
  for (size_t i = 0; i < tags.size(); ++i) {
    if (i) out += ", ";
    out += tags[i];
  }
  return out;
}

bool has_tag(const std::string& csv, const std::string& name) {
  for (auto& t : split_tags(csv))
    if (t == name) return true;
  return false;
}

std::string toggle_tag(const std::string& csv, const std::string& name) {
  auto tags = split_tags(csv);
  auto it = std::find(tags.begin(), tags.end(), name);
  if (it != tags.end())
    tags.erase(it);
  else
    tags.push_back(name);
  return join_tags(tags);
}

} // namespace eh::file_browser
