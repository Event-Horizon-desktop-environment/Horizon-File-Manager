// launch_path.cpp — see launch_path.hpp.
#include "app/file_browser/features/launch/launch_path.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace eh::file_browser {
namespace {

std::string trim_outer(const std::string& s) {
  size_t b = 0;
  while (b < s.size() &&
         (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
    ++b;
  size_t e = s.size();
  while (e > b &&
         (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
          s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percent_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hex_val(s[i + 1]);
      int lo = hex_val(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

}  // namespace

std::string normalize_launch_path(const std::string& in) {
  std::string s = trim_outer(in);
  if (s.empty()) return {};
  // file:// URIs (file:///path, file://localhost/path); scheme matched
  // case-insensitively, the rest percent-decoded.
  static constexpr size_t kSchemeLen = 7;  // strlen("file://")
  if (s.size() >= kSchemeLen) {
    std::string head = s.substr(0, kSchemeLen);
    for (auto& c : head)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (head == "file://") {
      std::string rest = percent_decode(trim_outer(s.substr(kSchemeLen)));
      // file://localhost/path and file:///path both mean /path.
      static constexpr const char* kLocalhost = "localhost/";
      if (rest.compare(0, 10, kLocalhost) == 0) rest.erase(0, 9);
      if (rest.empty() || rest[0] != '/') rest.insert(rest.begin(), '/');
      return rest;
    }
  }
  return s;
}

bool split_launch_target(const std::string& in, std::string& out_dir,
                         std::string& out_select) {
  out_dir.clear();
  out_select.clear();
  const std::string norm = normalize_launch_path(in);
  if (norm.empty()) return false;
  std::error_code ec;
  const fs::path p(norm);
  if (fs::is_directory(p, ec) && !ec) {
    out_dir = fs::path(p).lexically_normal().string();
    return true;
  }
  ec.clear();
  // A file (or a path that only existed briefly, e.g. a download renamed
  // between the browser click and our stat): open the containing folder.
  const fs::path parent =
      p.has_parent_path() ? p.parent_path() : fs::path("/");
  ec.clear();
  if (!parent.empty() && fs::is_directory(parent, ec) && !ec) {
    out_dir = fs::path(parent).lexically_normal().string();
    ec.clear();
    if (fs::exists(p, ec) && !ec)
      out_select = fs::path(p).lexically_normal().string();
    return true;
  }
  return false;
}

std::string find_cli_launch_target(int argc, char** argv) {
  static constexpr const char* kSelectEq = "--select=";
  for (int i = 1; i < argc; ++i) {
    const char* raw = argv[i];
    if (!raw || !*raw) continue;
    std::string a(raw);
    if (a == "--select-directory" || a == "--select-file") continue;
    if (a == "--select") {
      if (i + 1 < argc && argv[i + 1] && *argv[i + 1]) return argv[i + 1];
      return {};
    }
    if (a.compare(0, 9, kSelectEq) == 0) return a.substr(9);
    if (a[0] != '-') return a;
  }
  return {};
}

}  // namespace eh::file_browser
