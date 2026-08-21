#include "app/file_browser/features/query_match.hpp"

#include <fnmatch.h>

#include <regex>

namespace eh::file_browser {

namespace {

std::string to_lower_copy(const std::string& s) {
  std::string out(s);
  for (auto& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool plain_contains(const std::string& name, const std::string& query,
                    bool case_sensitive) {
  if (case_sensitive) return name.find(query) != std::string::npos;
  return to_lower_copy(name).find(to_lower_copy(query)) != std::string::npos;
}

} // namespace

bool name_matches(const std::string& name, const std::string& query,
                  int mode, bool case_sensitive, bool* valid) {
  if (valid) *valid = true;
  if (query.empty()) return false;

  switch (mode) {
    case static_cast<int>(QueryMode::Glob):
      return fnmatch(query.c_str(), name.c_str(),
                     case_sensitive ? 0 : FNM_CASEFOLD) == 0;

    case static_cast<int>(QueryMode::Regex): {
      try {
        auto flags = std::regex::ECMAScript;
        if (!case_sensitive) flags |= std::regex::icase;
        std::regex re(query, flags);
        return std::regex_search(name, re);
      } catch (const std::regex_error&) {
        if (valid) *valid = false;
        return false;
      }
    }

    case static_cast<int>(QueryMode::Content):
      // Name-level fallback; content matching happens in the search worker.
      [[fallthrough]];
    default:
      return plain_contains(name, query, case_sensitive);
  }
}

bool query_is_valid(const std::string& query, int mode) {
  if (mode != static_cast<int>(QueryMode::Regex) || query.empty()) return true;
  try {
    std::regex re(query, std::regex::ECMAScript);
    (void)re;
    return true;
  } catch (const std::regex_error&) {
    return false;
  }
}

} // namespace eh::file_browser
