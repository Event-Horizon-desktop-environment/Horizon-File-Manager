#pragma once

#include <string>

namespace eh::file_browser {

// Reads the freedesktop `user.xdg.tags` extended attribute (comma-separated
// tag list). Returns an empty string when the attribute is absent.
std::string read_xdg_tags(const std::string& path);

// Writes the `user.xdg.tags` extended attribute. An empty (or all-whitespace)
// value removes the attribute. Returns true on success.
bool write_xdg_tags(const std::string& path, const std::string& tags);

} // namespace eh::file_browser
