#include "view_zoom.hpp"

#include "../app_types.hpp"

#include <algorithm>
#include <cmath>

namespace eh::file_browser {

double zoom_pct_for_level(int level) {
  level = std::clamp(level, 0, kZoomLevelCount - 1);
  return std::round(50.0 * std::pow(2.0, static_cast<double>(level) / 8.0));
}

int zoom_level_for_pct(double pct) {
  int best = 0;
  double best_d = 1e9;
  for (int i = 0; i < kZoomLevelCount; ++i) {
    double d = std::abs(zoom_pct_for_level(i) - pct);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

void apply_zoom_pct(AppState& app, double pct) {
  app.settings_zoom_pct = std::clamp(pct, 50.0, 200.0);
  app.zoom_pct = app.settings_zoom_pct;
  app.entry_height =
      std::max(20, static_cast<int>(36.0 * app.zoom_pct / 100.0));
  int icon_sz = static_cast<int>(48.0 * app.zoom_pct / 100.0);
  app.grid_cell_size =
      std::max(40, icon_sz + static_cast<int>(8.0 * app.zoom_pct / 100.0));
  app.sidebar_width = std::max(
      120, static_cast<int>(app.sidebar_width_base * app.zoom_pct / 100.0));
}

void step_zoom(AppState& app, int delta) {
  apply_zoom_pct(app,
                 zoom_pct_for_level(zoom_level_for_pct(app.settings_zoom_pct) +
                                    delta));
}

}
