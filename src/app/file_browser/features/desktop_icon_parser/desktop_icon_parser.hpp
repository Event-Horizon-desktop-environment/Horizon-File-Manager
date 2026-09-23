#pragma once

#include <string>

namespace eh::file_browser {

// Parses a .desktop file and returns the value of the Icon= field
// from the [Desktop Entry] section. Returns empty string if the
// file cannot be read or no Icon= field is found.
std::string parse_desktop_icon(const std::string& path);

} // namespace eh::file_browser
