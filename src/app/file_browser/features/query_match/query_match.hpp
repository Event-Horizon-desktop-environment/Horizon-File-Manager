#pragma once

#include <string>

namespace eh::file_browser {

// Filter-bar query modes (Dolphin-style).
enum class QueryMode {
  Plain = 0, // substring
  Glob = 1,  // shell wildcards (* ? [..])
  Regex = 2, // ECMAScript regular expression
  Content = 3, // match inside file contents (worker-side), name falls back to plain
};

// Match a file name against the query. Never throws: an invalid regex simply
// does not match and reports *valid == false when provided.
bool name_matches(const std::string& name, const std::string& query,
                  int mode, bool case_sensitive, bool* valid = nullptr);

// Validate without matching (used for live red-invalid feedback).
bool query_is_valid(const std::string& query, int mode);

} // namespace eh::file_browser
