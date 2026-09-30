// ignore.cpp — gitignore matching (see ignore.hpp).

#include "ignore.hpp"

#include <cstdio>
#include <cstring>

namespace hgrep {
namespace {

// '*' matches anything except '/', '?' matches one non-'/' char,
// "**/" matches zero or more path components.
bool match_here(const char* pat, const char* str) {
  for (;;) {
    if (*pat == '\0') return *str == '\0';
    if (pat[0] == '*' && pat[1] == '*') {
      // Collapse runs of stars; "**/" eats whole components.
      while (*pat == '*') ++pat;
      if (*pat == '/') {
        ++pat;
        // zero components…
        if (match_here(pat, str)) return true;
        // …or consume one component at a time.
        while (*str) {
          while (*str && *str != '/') ++str;
          if (*str == '/') {
            ++str;
            if (match_here(pat, str)) return true;
          }
        }
        return false;
      }
      // Trailing "**" matches everything left.
      if (*pat == '\0') return true;
      while (*str) {
        if (match_here(pat, str)) return true;
        ++str;
      }
      return match_here(pat, str);
    }
    if (*pat == '*') {
      ++pat;
      if (*pat == '\0') {
        while (*str && *str != '/') ++str;
        return *str == '\0';
      }
      while (*str && *str != '/') {
        if (match_here(pat, str)) return true;
        ++str;
      }
      return match_here(pat, str);
    }
    if (*pat == '?') {
      if (*str == '\0' || *str == '/') return false;
      ++pat;
      ++str;
      continue;
    }
    if (*pat == '/') {
      if (*str != '/') return false;
      ++pat;
      ++str;
      continue;
    }
    if (*pat != *str) return false;
    ++pat;
    ++str;
  }
}

} // namespace

bool glob_match(const char* pat, const char* str) {
  return match_here(pat, str);
}

void IgnoreStack::push_dir(const std::string& dir, const std::string& rel) {
  IgnoreLevel level;
  level.prefix = rel;
  for (const char* name : {".gitignore", ".ignore"}) {
    std::string path = dir;
    if (!path.empty() && path.back() != '/') path += '/';
    path += name;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) continue;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
      std::string line = buf;
      while (!line.empty() &&
             (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      IgnoreLevel::Rule rule;
      if (line[0] == '!') {
        rule.negate = true;
        line.erase(line.begin());
        if (line.empty()) continue;
      }
      if (!line.empty() && line.back() == '/') {
        rule.dir_only = true;
        line.pop_back();
      }
      if (!line.empty() && line[0] == '/') line.erase(line.begin());
      if (line.empty()) continue;
      rule.anchored = line.find('/') != std::string::npos;
      rule.pattern = std::move(line);
      level.rules.push_back(std::move(rule));
    }
    fclose(f);
  }
  levels_.push_back(std::move(level));
}

void IgnoreStack::pop_dir() {
  if (!levels_.empty()) levels_.pop_back();
}

bool IgnoreStack::ignored(const std::string& rel, bool is_dir) const {
  for (size_t li = levels_.size(); li-- > 0;) {
    const auto& level = levels_[li];
    bool decided = false, value = false;
    for (auto& r : level.rules) {
      if (r.dir_only && !is_dir) continue;
      bool hit = false;
      if (r.anchored) {
        // Anchored at the file's own directory.
        const std::string& pre = level.prefix;
        if (pre.empty()) {
          hit = glob_match(r.pattern.c_str(), rel.c_str());
        } else if (rel.size() > pre.size() &&
                   rel.compare(0, pre.size(), pre) == 0 &&
                   rel[pre.size()] == '/') {
          hit = glob_match(r.pattern.c_str(), rel.c_str() + pre.size() + 1);
        }
      } else {
        // Basename match at any depth.
        const char* base = rel.c_str();
        const char* slash = strrchr(base, '/');
        const char* name = slash ? slash + 1 : base;
        if (glob_match(r.pattern.c_str(), name)) {
          hit = true;
        } else if (!is_dir) {
          // "foo" style rules also match "foo/bar" paths' ancestors
          // only via dir pruning in the walker; files match by name.
        }
      }
      if (hit) {
        decided = true;
        value = !r.negate;
      }
    }
    if (decided) return value;
  }
  return false;
}

} // namespace hgrep
