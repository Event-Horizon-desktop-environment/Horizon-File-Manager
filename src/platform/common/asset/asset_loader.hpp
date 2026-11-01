#pragma once

#include <cairo/cairo.h>
#include <string>

namespace eh::shell::asset {

cairo_surface_t* load_brand_logo_surface();
cairo_surface_t* load_launchpad_logo_surface(bool light = false);
cairo_surface_t* load_trash_empty_surface();
cairo_surface_t* load_trash_full_surface();

/// Load an SVG file from the standard asset search paths, looking under a subdirectory.
/// Searches EH_ASSETS_DIR, paths relative to the executable and the working
/// directory (so build trees work without installing), and the system install
/// dirs under horizon-files/ and event-horizon/.
/// `subdir` is e.g. "UI" (no leading/trailing slash), `filename` e.g. "arrow-left.svg".
/// Returns nullptr if not found or on error.
cairo_surface_t* load_asset_svg(const char* subdir, const char* filename, int max_px);

}
