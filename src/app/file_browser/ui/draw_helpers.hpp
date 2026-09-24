#pragma once

// draw_helpers.hpp — popup surface tinting and dialog card painting.
// Exported from ui/draw.cpp as part of the Step 3 file split.

#include "../app.hpp"

#include <cairo/cairo.h>

namespace eh::file_browser {

// Strength used to blend every popup surface toward the wallpaper-derived
// accent, so popups take colors from the active wallpaper palette.
inline constexpr double kPopupWallpaperTint = 0.35;

// Dim the tinted surface slightly so popup cards read a bit darker.
inline constexpr double kPopupTintDarken = 0.9;

// Compute the tinted RGB values for a popup surface based on the active
// wallpaper palette.
void wallpaper_tint_surface(const AppState& app, double strength,
                            double& out_r, double& out_g, double& out_b);

// Layered soft shadow + solid card + hairline border shared by every popup.
// `alpha` tints only the card fill (content stays crisp); popups on the
// main surface keep the opaque default.
void draw_dialog_card(AppState& app, cairo_t* cr, double x, double y,
                      double w, double h, double r, double alpha = 1.0);

// Clamp a popup rect so it stays fully on-screen (with an 8px margin).
// Shared by the context menu and the drop-action chooser.
void clamp_popup_rect(const AppState& app, int& x, int& y, int w, int h);

} // namespace eh::file_browser
