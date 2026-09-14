#pragma once

// draw_file_icons.hpp — icon-name lookup and file-icon drawing.
// Exported from ui/draw.cpp as part of the Step 3 file split.

#include "../app.hpp"

#include <cairo/cairo.h>
#include <string>

namespace eh::file_browser {

class BatchedBlitter;

// Map a FileType enum (and optional path for PDF detection) to an
// icon-theme string name.
const char* icon_name_for_file_type(FileType ft,
                                    const std::string* file_path = nullptr);

// Draw a complete file icon with thumbnail, type glyph and status badge.
void draw_file_icon_cairo(AppState& app, cairo_t* cr,
                          int x, int y, int size,
                          FileType ft, bool selected,
                          const std::string& icon_name = {},
                          cairo_surface_t* thumb = nullptr,
                          const std::string* file_path = nullptr,
                          const FileEntry* entry = nullptr,
                          BatchedBlitter* bb = nullptr);

} // namespace eh::file_browser
