#pragma once

namespace eh::file_browser {

struct AppState;

constexpr int kZoomLevelCount = 33; // 50%..200%, geometric 50·2^(n/16) ≈ 4.4%/step

double zoom_pct_for_level(int level);
int zoom_level_for_pct(double pct);
void apply_zoom_pct(AppState& app, double pct);
void step_zoom(AppState& app, int delta);

}
