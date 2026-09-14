#pragma once

// draw_thumbnails.hpp — thumbnail cache lookup and lazy enqueue.
// Exported from ui/draw.cpp as part of the Step 3 file split.

#include "../app.hpp"

#include <cairo/cairo.h>
#include <string>

namespace eh::file_browser {

// Return the cached thumbnail for `path`, or nullptr if not yet decoded.
// Never blocks — queues background decode on miss.
cairo_surface_t* get_thumbnail_lazy(AppState& app, int vi,
                                    const std::string& path, int size);

} // namespace eh::file_browser
