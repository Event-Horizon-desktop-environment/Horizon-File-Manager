// thumb_defer.cpp — see thumb_defer.hpp.
#include "app/file_browser/features/thumbnails/thumb_defer.hpp"

#include <cctype>
#include <ctime>
#include <string>

#include <sys/stat.h>

namespace eh::file_browser {
namespace {

// Files at/above this size whose mtime is this fresh are assumed to be
// mid-write: decoding them now burns a worker for little benefit.
constexpr long long kLargeBytes = 64LL * 1024 * 1024;
constexpr long long kFreshSecs = 30;

// Partial-download suffixes. Browsers/helpers write either alongside the
// final name (foo.crdownload) or as the only name on disk.
constexpr const char* kPartialSuffixes[] = {
    ".part",     ".crdownload", ".partial",  ".download", ".tmp",
    ".!ut",      ".opdownload", ".fdmdownload", ".aria2",  ".filepart",
    ".td",
};

bool has_partial_suffix(const std::string& lower_path) {
  for (const char* suf : kPartialSuffixes) {
    size_t n = 0;
    while (suf[n]) ++n;
    if (lower_path.size() >= n &&
        lower_path.compare(lower_path.size() - n, n, suf) == 0)
      return true;
  }
  return false;
}

}  // namespace

bool thumb_should_defer(const std::string& path) {
  if (path.empty()) return false;
  std::string lower;
  lower.reserve(path.size());
  for (char c : path)
    lower.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  if (has_partial_suffix(lower)) return true;

  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return false;
  if (!S_ISREG(st.st_mode)) return false;
  if (static_cast<long long>(st.st_size) < kLargeBytes) return false;
  const long long now = static_cast<long long>(std::time(nullptr));
  const long long age = now - static_cast<long long>(st.st_mtime);
  // Clamp clock weirdness: a future mtime means "just written", defer.
  if (age < 0) return true;
  return age <= kFreshSecs;
}

}  // namespace eh::file_browser
