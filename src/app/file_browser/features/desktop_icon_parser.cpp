#include "app/file_browser/features/desktop_icon_parser.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace eh::file_browser {

std::string parse_desktop_icon(const std::string& path) {
  FILE* fp = fopen(path.c_str(), "r");
  if (!fp) return {};

  char line[512];
  bool in_entry = false;
  std::string icon;

  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "[Desktop Entry]", 15) == 0) {
      in_entry = true;
      continue;
    }
    if (!in_entry) continue;
    if (line[0] == '[' || line[0] == '\n') break;
    if (strncmp(line, "Icon=", 5) != 0) continue;

    icon.assign(line + 5);
    while (!icon.empty() && (icon.back() == '\n' || icon.back() == '\r' ||
                             icon.back() == ' ' || icon.back() == '\t'))
      icon.pop_back();
    break;
  }

  fclose(fp);
  return icon;
}

} // namespace eh::file_browser
