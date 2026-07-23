#include "platform/common/palette/matugen_palette.hpp"
#include "config/shell_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

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

  appearance.matugenPaletteOk = true;
}

}
