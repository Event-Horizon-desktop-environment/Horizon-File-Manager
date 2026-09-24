// draw_helpers.cpp — shared popup surface tinting and dialog card painting.
// Moved wholesale from ui/draw.cpp (byte-identical bodies).

#include "../app.hpp"
#include "draw_helpers.hpp"

#include <cairo/cairo.h>

namespace fs = std::filesystem;

namespace eh::file_browser {

void wallpaper_tint_surface(const AppState& app, double strength,
                                   double& out_r, double& out_g, double& out_b) {
  out_r = (app.surface_r * (1.0 - strength) + app.accent_r * strength) * kPopupTintDarken;
  out_g = (app.surface_g * (1.0 - strength) + app.accent_g * strength) * kPopupTintDarken;
  out_b = (app.surface_b * (1.0 - strength) + app.accent_b * strength) * kPopupTintDarken;
}

// Layered soft shadow + solid card + hairline border shared by every popup.
// Cards are always fully opaque (no transparency anywhere in dialogs).
void draw_dialog_card(AppState& app, cairo_t* cr, double x, double y,
                             double w, double h, double r, double alpha) {
  for (int s = 4; s >= 0; --s) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0.05 * (5 - s));
    draw_rounded_rect(cr, x + s * 1.5, y + s * 2.5, w, h, r);
    cairo_fill(cr);
  }
  double tint_r, tint_g, tint_b;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tint_r, tint_g, tint_b);
  cairo_set_source_rgba(cr, tint_r, tint_g, tint_b, alpha);
  draw_rounded_rect(cr, x, y, w, h, r);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.22);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r - 0.5);
  cairo_stroke(cr);
}

// Clamp a popup rect so it stays fully on-screen (with an 8px margin).
void clamp_popup_rect(const AppState& app, int& x, int& y, int w, int h) {
  if (x + w > app.width - 8) x = app.width - w - 8;
  if (y + h > app.height - 8) y = app.height - h - 8;
  if (x < 8) x = 8;
  if (y < 8) y = 8;
}

} // namespace eh::file_browser
