#include "platform/common/palette/matugen_palette.hpp"
#include "platform/common/palette/horizon_colors.hpp"
#include "config/shell_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

namespace eh::matugen {

std::string normalize_matugen_scheme(std::string_view in) {
  return std::string(in);
}

std::string normalize_matugen_mode(std::string_view in) {
  return std::string(in);
}

static bool hex_to_rgb(const std::string& hex, float& r, float& g, float& b) {
  std::string h = hex;
  if (!h.empty() && h[0] == '#') h = h.substr(1);
  if (h.size() != 6) return false;
  try {
    unsigned long val = std::stoul(h, nullptr, 16);
    r = ((val >> 16) & 0xFF) / 255.0f;
    g = ((val >> 8) & 0xFF) / 255.0f;
    b = (val & 0xFF) / 255.0f;
    return true;
  } catch (...) { return false; }
}

static std::string find_dark_color(const std::string& json, const std::string& name) {
  std::string needle = "\"" + name + "\"";
  auto pos = json.find(needle);
  if (pos == std::string::npos) return {};
  pos = json.find("\"dark\"", pos);
  if (pos == std::string::npos) return {};
  pos = json.find("\"color\"", pos);
  if (pos == std::string::npos) return {};
  auto colon = json.find(':', pos);
  if (colon == std::string::npos) return {};
  auto quote1 = json.find('"', colon + 1);
  if (quote1 == std::string::npos) return {};
  auto quote2 = json.find('"', quote1 + 1);
  if (quote2 == std::string::npos) return {};
  return json.substr(quote1 + 1, quote2 - quote1 - 1);
}

void refresh_wallpaper_derived_palette(eh::config::ShellAppearance& appearance,
                                         const std::string& normalized_wallpaper_image_path) {
  if (!appearance.matugenThemingEnabled) {
    appearance.matugenPaletteOk = false;
    return;
  }

  if (normalized_wallpaper_image_path.empty()) {
    appearance.matugenPaletteOk = false;
    return;
  }

  // ── Native color engine (preferred when available) ────────────────────────
  if (appearance.horizonColorsNative) {
    const bool is_dark = (appearance.matugenMode != "light");
    const auto variant = eh::color::scheme_variant_from_name(appearance.matugenScheme);
    eh::color::PaletteResult result =
        eh::color::generate_palette_from_image(normalized_wallpaper_image_path, variant, is_dark);
    if (result.ok) {
      appearance.matugenDockFillR = result.dockFillR;
      appearance.matugenDockFillG = result.dockFillG;
      appearance.matugenDockFillB = result.dockFillB;
      appearance.matugenPanelFillR = result.panelFillR;
      appearance.matugenPanelFillG = result.panelFillG;
      appearance.matugenPanelFillB = result.panelFillB;
      appearance.matugenDrawerDimR = result.drawerDimR;
      appearance.matugenDrawerDimG = result.drawerDimG;
      appearance.matugenDrawerDimB = result.drawerDimB;
      appearance.matugenOutlineR = result.outlineR;
      appearance.matugenOutlineG = result.outlineG;
      appearance.matugenOutlineB = result.outlineB;
      appearance.matugenAccentR = result.accentR;
      appearance.matugenAccentG = result.accentG;
      appearance.matugenAccentB = result.accentB;
      // Text is always pure white in dark, pure black in light
      appearance.matugenTextR = is_dark ? 1.0f : 0.0f;
      appearance.matugenTextG = is_dark ? 1.0f : 0.0f;
      appearance.matugenTextB = is_dark ? 1.0f : 0.0f;
      appearance.matugenNotifCriticalBgR = result.notifCriticalBgR;
      appearance.matugenNotifCriticalBgG = result.notifCriticalBgG;
      appearance.matugenNotifCriticalBgB = result.notifCriticalBgB;
      appearance.matugenNotifCriticalOutlineR = result.notifCriticalOutlineR;
      appearance.matugenNotifCriticalOutlineG = result.notifCriticalOutlineG;
      appearance.matugenNotifCriticalOutlineB = result.notifCriticalOutlineB;

      // Full 47-role M3 palette (indices match m3::ColorRole / kM3RoleNames)
      for (unsigned ri = 0; ri < eh::config::kM3RoleCount; ++ri) {
        auto it = result.roles.find(static_cast<uint8_t>(ri));
        if (it == result.roles.end()) continue;
        const eh::color::Argb c = it->second;
        appearance.m3Palette.rgb[ri][0] = eh::color::red_from_argb(c) / 255.0f;
        appearance.m3Palette.rgb[ri][1] = eh::color::green_from_argb(c) / 255.0f;
        appearance.m3Palette.rgb[ri][2] = eh::color::blue_from_argb(c) / 255.0f;
      }
      appearance.m3Palette.loaded = true;

      appearance.matugenPaletteOk = true;
      return;
    }
    // Native engine failed — fall through to matugen CLI
  }

  // ── Legacy matugen CLI fallback ───────────────────────────────────────────

  std::string cmd = "matugen image \"" + normalized_wallpaper_image_path +
                    "\" --mode " + appearance.matugenMode +
                    " --type " + appearance.matugenScheme +
                    " --json hex --dry-run --source-color-index 0 2>/dev/null";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    appearance.matugenPaletteOk = false;
    return;
  }

  std::string json_output;
  char buf[4096];
  while (fgets(buf, sizeof(buf), pipe)) json_output += buf;
  int status = pclose(pipe);

  if (status != 0 || json_output.empty() || json_output.find("\"colors\"") == std::string::npos) {
    appearance.matugenPaletteOk = false;
    return;
  }

  auto set_color = [&](const std::string& name, float& r, float& g, float& b) {
    std::string hex = find_dark_color(json_output, name);
    if (!hex.empty()) hex_to_rgb(hex, r, g, b);
  };

  set_color("surface_container", appearance.matugenDockFillR, appearance.matugenDockFillG, appearance.matugenDockFillB);
  set_color("surface", appearance.matugenPanelFillR, appearance.matugenPanelFillG, appearance.matugenPanelFillB);
  set_color("surface_dim", appearance.matugenDrawerDimR, appearance.matugenDrawerDimG, appearance.matugenDrawerDimB);
  set_color("outline", appearance.matugenOutlineR, appearance.matugenOutlineG, appearance.matugenOutlineB);
  set_color("primary", appearance.matugenAccentR, appearance.matugenAccentG, appearance.matugenAccentB);
  set_color("on_surface", appearance.matugenTextR, appearance.matugenTextG, appearance.matugenTextB);
  set_color("error_container", appearance.matugenNotifCriticalBgR, appearance.matugenNotifCriticalBgG, appearance.matugenNotifCriticalBgB);
  set_color("error", appearance.matugenNotifCriticalOutlineR, appearance.matugenNotifCriticalOutlineG, appearance.matugenNotifCriticalOutlineB);

  // Scrape the full M3 role set from the JSON output
  for (unsigned ri = 0; ri < eh::config::kM3RoleCount; ++ri) {
    std::string hex = find_dark_color(json_output, eh::config::kM3RoleNames[ri]);
    float r, g, b;
    if (!hex_to_rgb(hex, r, g, b)) continue;
    appearance.m3Palette.rgb[ri][0] = r;
    appearance.m3Palette.rgb[ri][1] = g;
    appearance.m3Palette.rgb[ri][2] = b;
    appearance.m3Palette.loaded = true;
  }

  appearance.matugenPaletteOk = true;
}

// ── Read Event Horizon Shell color engine output ────────────────────────────
// Parses ~/.config/event-horizon/horizon-files-matugen.conf written by the
// shell's native color engine template system.  Returns true when colors
// were successfully loaded.

static std::string config_dir_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/event-horizon";
  const char* home = std::getenv("HOME");
  return home ? std::string(home) + "/.config/event-horizon" : ".config/event-horizon";
}

std::string shell_color_config_path() {
  return config_dir_path() + "/horizon-files-matugen.conf";
}

bool read_shell_color_config(eh::config::ShellAppearance& appearance) {
  std::string path = shell_color_config_path();
  std::ifstream in(path);
  if (!in) return false;

  std::unordered_map<std::string, std::string> hex_map;
  std::string ln;
  while (std::getline(in, ln)) {
    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
    if (ln.empty() || ln[0] == '#') continue;
    auto eq = ln.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string key = ln.substr(0, eq);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    std::string val = (eq + 1 < ln.size()) ? ln.substr(eq + 1) : "";
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
      val = val.substr(1, val.size() - 2);
    if (!val.empty()) hex_map[key] = val;
  }
  if (hex_map.empty()) return false;

  auto apply = [&](const std::string& key, float& r, float& g, float& b) {
    auto it = hex_map.find(key);
    if (it != hex_map.end())
      hex_to_rgb(it->second, r, g, b);
  };

  apply("dock_fill",              appearance.matugenDockFillR,           appearance.matugenDockFillG,           appearance.matugenDockFillB);
  apply("panel_fill",             appearance.matugenPanelFillR,          appearance.matugenPanelFillG,          appearance.matugenPanelFillB);
  apply("drawer_dim",             appearance.matugenDrawerDimR,          appearance.matugenDrawerDimG,          appearance.matugenDrawerDimB);
  apply("outline",                appearance.matugenOutlineR,            appearance.matugenOutlineG,            appearance.matugenOutlineB);
  apply("accent",                 appearance.matugenAccentR,             appearance.matugenAccentG,             appearance.matugenAccentB);
  apply("text",                   appearance.matugenTextR,               appearance.matugenTextG,               appearance.matugenTextB);
  apply("notif_critical_bg",      appearance.matugenNotifCriticalBgR,    appearance.matugenNotifCriticalBgG,    appearance.matugenNotifCriticalBgB);
  apply("notif_critical_outline", appearance.matugenNotifCriticalOutlineR, appearance.matugenNotifCriticalOutlineG, appearance.matugenNotifCriticalOutlineB);

  // Full M3 role set emitted by the shell's native color engine template
  for (unsigned ri = 0; ri < eh::config::kM3RoleCount; ++ri) {
    auto it = hex_map.find(eh::config::kM3RoleNames[ri]);
    if (it == hex_map.end()) continue;
    float r, g, b;
    if (!hex_to_rgb(it->second, r, g, b)) continue;
    appearance.m3Palette.rgb[ri][0] = r;
    appearance.m3Palette.rgb[ri][1] = g;
    appearance.m3Palette.rgb[ri][2] = b;
    appearance.m3Palette.loaded = true;
  }

  appearance.matugenPaletteOk = true;
  return true;
}

}
