#pragma once

#include <map>
#include <string>
#include <vector>

namespace eh::file_browser {

// Reads the freedesktop `user.xdg.tags` extended attribute (comma-separated
// tag list). Returns an empty string when the attribute is absent.
std::string read_xdg_tags(const std::string& path);

// Writes the `user.xdg.tags` extended attribute. An empty (or all-whitespace)
// value removes the attribute. Returns true on success.
bool write_xdg_tags(const std::string& path, const std::string& tags);


struct TagDef {
  std::string name;
  double r = 0.5, g = 0.5, b = 0.5; // sRGB 0..1
};

// Built-in set (colors are defaults; user choices persist in config).
std::vector<TagDef> default_tags();

// All known tags: defaults with config colors applied. Unknown (custom)
// tag names are NOT included — callers render those neutral gray.
std::vector<TagDef> tag_registry(const std::map<std::string, std::string>& color_overrides);

// Hex "#rrggbb" (or "#rgb", with/without '#') -> rgb. False when malformed.
bool parse_hex_color(const std::string& hex, double& r, double& g, double& b);
std::string hex_color(double r, double g, double b);

// Split "a, b,c" -> ["a","b","c"] (trimmed, empties dropped).
std::vector<std::string> split_tags(const std::string& csv);
// Join back with ", ".
std::string join_tags(const std::vector<std::string>& tags);
// Toggle one tag in a csv list (add when absent, remove when present).
std::string toggle_tag(const std::string& csv, const std::string& name);
// True when name is in the csv list.
bool has_tag(const std::string& csv, const std::string& name);

// Rating 0–5 stored in `user.xdg.rating` (single byte "0".."5"; absent = 0).
// Clamped on read; UNSET (-1) only used internally for write-through errors.
int read_xdg_rating(const std::string& path);

// Writes the rating (0 clears the attribute). Returns true on success.
bool write_xdg_rating(const std::string& path, int rating);

// Free-text comment in `user.xdg.comment` (empty removes it). Capped at
// 1024 bytes. Returns the stored value ("" = none).
std::string read_xdg_comment(const std::string& path);

// Writes the comment. Returns true on success.
bool write_xdg_comment(const std::string& path, const std::string& comment);

} // namespace eh::file_browser
