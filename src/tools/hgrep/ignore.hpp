#pragma once

// ignore.hpp — .gitignore/.ignore stacking (minimal git subset).
//
// Supported: *, ?, **, trailing '/' (dirs only), leading '/' (anchored),
// '!' negation. A pattern without '/' matches basenames at any depth;
// otherwise it matches the repo-relative path. Deeper files override
// shallower ones; within a file the last matching line wins.

#include <string>
#include <vector>

namespace hgrep {

struct IgnoreLevel {
  struct Rule {
    std::string pattern;
    bool negate = false;
    bool dir_only = false;
    bool anchored = false; // contains '/' (after stripping lead/trail)
  };
  std::string prefix; // root-relative dir owning this file ("" at root)
  std::vector<Rule> rules;
};

class IgnoreStack {
 public:
  // Load .gitignore then .ignore from dir (missing files are fine).
  // rel is the root-relative path of dir ("" at the root itself).
  void push_dir(const std::string& dir, const std::string& rel);
  void pop_dir();
  // rel uses '/' separators, relative to the search root.
  bool ignored(const std::string& rel, bool is_dir) const;

 private:
  std::vector<IgnoreLevel> levels_;
};

bool glob_match(const char* pat, const char* str);

} // namespace hgrep
