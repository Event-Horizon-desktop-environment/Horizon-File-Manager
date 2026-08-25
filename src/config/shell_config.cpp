#define TOML_IMPLEMENTATION
#include "config/shell_config.hpp"
#include <toml++/toml.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace eh::config {

namespace fs = std::filesystem;

// ── path helpers ──────────────────────────────────────────────────────

static std::string config_dir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/event-horizon";
  const char* home = std::getenv("HOME");
  return home ? std::string(home) + "/.config/event-horizon" : ".config/event-horizon";
}

static std::string state_dir() {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/event-horizon";
  const char* home = std::getenv("HOME");
  return home ? std::string(home) + "/.local/state/event-horizon" : ".local/state/event-horizon";
}

static std::string file_browser_toml_path() {
  return config_dir() + "/file-browser.toml";
}

std::string state_file_browser_toml_path() {
  return file_browser_toml_path();
}

std::string state_settings_toml_path() {
  return config_dir() + "/state-settings.toml";
}

std::string legacy_ini_path() {
  return config_dir() + "/settings.ini";
}

// ── singleton config snapshot ────────────────────────────────────────

static ShellConfig g_snapshot;

const ShellConfig& shell_config_snapshot_skip_matugen() {
  return g_snapshot;
}

const ShellConfig& shell_config_snapshot() {
  return g_snapshot;
}

void shell_config_reload_from_disk_now() {
  // "primary_fixed_dim" -> "matugenPrimaryFixedDimR" style TOML keys
  static auto matugen_toml_key = [](std::string_view role, char comp) {
    std::string k = "matugen";
    bool cap = true;
    for (char c : role) {
      if (c == '_') {
        cap = true;
        continue;
      }
      k.push_back(cap ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
      cap = false;
    }
    k.push_back(comp);
    return k;
  };

  // Read state-settings.toml into g_snapshot
  std::string path = state_settings_toml_path();
  std::ifstream f(path);
  if (!f.is_open()) return;

  try {
    toml::table tbl = toml::parse(f);

    // appearance
    if (auto* ap = tbl["appearance"].as_table()) {
      auto& a = g_snapshot.appearance;
      if (auto* v = ap->get("matugenThemingEnabled")) a.matugenThemingEnabled = v->value_or(false);
      if (auto* v = ap->get("colorEngineEnabled")) a.colorEngineEnabled = v->value_or(true);
      if (auto* v = ap->get("horizonColorsNative")) a.horizonColorsNative = v->value_or(true);
      if (auto* v = ap->get("matugenPaletteOk")) a.matugenPaletteOk = v->value_or(false);
      if (auto* v = ap->get("matugenScheme")) a.matugenScheme = v->value_or("scheme-content");
      if (auto* v = ap->get("matugenMode")) a.matugenMode = v->value_or("dark");
      if (auto* v = ap->get("colorBrightness")) a.colorBrightness = v->value_or(1.0f);
      if (auto* v = ap->get("colorContrast")) a.colorContrast = v->value_or(1.0f);
      if (auto* v = ap->get("colorVibrance")) a.colorVibrance = v->value_or(1.0f);
      if (auto* v = ap->get("colorGamma")) a.colorGamma = v->value_or(1.0f);
      if (auto* v = ap->get("matugenAccentR")) a.matugenAccentR = v->value_or(0.90f);
      if (auto* v = ap->get("matugenAccentG")) a.matugenAccentG = v->value_or(0.90f);
      if (auto* v = ap->get("matugenAccentB")) a.matugenAccentB = v->value_or(0.90f);
      if (auto* v = ap->get("matugenTextR")) a.matugenTextR = v->value_or(0.92f);
      if (auto* v = ap->get("matugenTextG")) a.matugenTextG = v->value_or(0.92f);
      if (auto* v = ap->get("matugenTextB")) a.matugenTextB = v->value_or(0.95f);
      if (auto* v = ap->get("matugenPanelFillR")) a.matugenPanelFillR = v->value_or(0.08f);
      if (auto* v = ap->get("matugenPanelFillG")) a.matugenPanelFillG = v->value_or(0.10f);
      if (auto* v = ap->get("matugenPanelFillB")) a.matugenPanelFillB = v->value_or(0.13f);
      if (auto* v = ap->get("matugenOutlineR")) a.matugenOutlineR = v->value_or(0.55f);
      if (auto* v = ap->get("matugenOutlineG")) a.matugenOutlineG = v->value_or(0.60f);
      if (auto* v = ap->get("matugenOutlineB")) a.matugenOutlineB = v->value_or(0.62f);
      if (auto* v = ap->get("matugenDockFillR")) a.matugenDockFillR = v->value_or(0.12f);
      if (auto* v = ap->get("matugenDockFillG")) a.matugenDockFillG = v->value_or(0.12f);
      if (auto* v = ap->get("matugenDockFillB")) a.matugenDockFillB = v->value_or(0.14f);
      if (auto* v = ap->get("matugenDrawerDimR")) a.matugenDrawerDimR = v->value_or(0.11f);
      if (auto* v = ap->get("matugenDrawerDimG")) a.matugenDrawerDimG = v->value_or(0.11f);
      if (auto* v = ap->get("matugenDrawerDimB")) a.matugenDrawerDimB = v->value_or(0.13f);
      if (auto* v = ap->get("matugenNotifCriticalBgR")) a.matugenNotifCriticalBgR = v->value_or(0.18f);
      if (auto* v = ap->get("matugenNotifCriticalBgG")) a.matugenNotifCriticalBgG = v->value_or(0.06f);
      if (auto* v = ap->get("matugenNotifCriticalBgB")) a.matugenNotifCriticalBgB = v->value_or(0.06f);
      if (auto* v = ap->get("matugenNotifCriticalOutlineR")) a.matugenNotifCriticalOutlineR = v->value_or(0.70f);
      if (auto* v = ap->get("matugenNotifCriticalOutlineG")) a.matugenNotifCriticalOutlineG = v->value_or(0.10f);
      if (auto* v = ap->get("matugenNotifCriticalOutlineB")) a.matugenNotifCriticalOutlineB = v->value_or(0.10f);

      // Full M3 palette written by the shell's native color engine post-hook
      // (matugen<Role>R/G/B, see sync_horizon_files in horizon_colors_templates.cpp)
      bool m3_any = false;
      for (unsigned ri = 0; ri < kM3RoleCount; ++ri) {
        static constexpr char kComps[3] = {'R', 'G', 'B'};
        for (int ci = 0; ci < 3; ++ci) {
          std::string key = matugen_toml_key(kM3RoleNames[ri], kComps[ci]);
          if (auto* v = ap->get(key)) {
            a.m3Palette.rgb[ri][ci] = v->value_or(0.0f);
            m3_any = true;
          }
        }
      }
      a.m3Palette.loaded = m3_any;
    }

    // dock
    if (auto* dock = tbl["dock"].as_table()) {
      if (auto* v = dock->get("iconTheme")) g_snapshot.dock.iconTheme = v->value_or("");
    }

    // defaultApps
    if (auto* da = tbl["defaultApps"].as_table()) {
      if (auto* v = da->get("terminal")) g_snapshot.defaultApps.terminal = v->value_or("");
    }

    if (auto* v = tbl["wallpaperImage"].as_string()) g_snapshot.wallpaperImage = v->get();
  } catch (const std::exception& e) {
    std::cerr << "[horizon-files] TOML parse error: " << e.what() << "\n";
  }

  // Also read wallpaper image from the component file if not in state-settings
  if (g_snapshot.wallpaperImage.empty()) {
    try {
      std::string wp_path = state_dir() + "/wallpaper/wallpaper.toml";
      std::ifstream wf(wp_path);
      if (wf.is_open()) {
        toml::table wtbl = toml::parse(wf);
        if (auto* sec = wtbl["wallpaper"].as_table()) {
          if (auto* v = sec->get("image")) g_snapshot.wallpaperImage = v->value_or("");
        }
      }
    } catch (...) {}
  }
}

void shell_config_apply_from_memory(ShellConfig sc) {
  g_snapshot = std::move(sc);
}

// ── chrome colors ────────────────────────────────────────────────────

ChromePaintColors derived_chrome_colors(const ShellAppearance& appearance) {
  ChromePaintColors mc;
  const bool palette_active =
      (appearance.colorEngineEnabled || appearance.matugenThemingEnabled) && appearance.matugenPaletteOk;
  if (palette_active) {
    mc.accentR = appearance.matugenAccentR;
    mc.accentG = appearance.matugenAccentG;
    mc.accentB = appearance.matugenAccentB;
    mc.textR = appearance.matugenTextR;
    mc.textG = appearance.matugenTextG;
    mc.textB = appearance.matugenTextB;
    mc.panelFillR = appearance.matugenPanelFillR;
    mc.panelFillG = appearance.matugenPanelFillG;
    mc.panelFillB = appearance.matugenPanelFillB;
    mc.outlineR = appearance.matugenOutlineR;
    mc.outlineG = appearance.matugenOutlineG;
    mc.outlineB = appearance.matugenOutlineB;
    mc.dockFillR = appearance.matugenDockFillR;
    mc.dockFillG = appearance.matugenDockFillG;
    mc.dockFillB = appearance.matugenDockFillB;
    mc.drawerDimR = appearance.matugenDrawerDimR;
    mc.drawerDimG = appearance.matugenDrawerDimG;
    mc.drawerDimB = appearance.matugenDrawerDimB;
    mc.notifCriticalBgR = appearance.matugenNotifCriticalBgR;
    mc.notifCriticalBgG = appearance.matugenNotifCriticalBgG;
    mc.notifCriticalBgB = appearance.matugenNotifCriticalBgB;
    mc.notifCriticalOutlineR = appearance.matugenNotifCriticalOutlineR;
    mc.notifCriticalOutlineG = appearance.matugenNotifCriticalOutlineG;
    mc.notifCriticalOutlineB = appearance.matugenNotifCriticalOutlineB;
  } else {
    mc.accentR = 0.30; mc.accentG = 0.58; mc.accentB = 0.90;
    mc.textR = 0.94; mc.textG = 0.94; mc.textB = 0.96;
    mc.panelFillR = 0.08; mc.panelFillG = 0.08; mc.panelFillB = 0.10;
    mc.outlineR = 0.06; mc.outlineG = 0.06; mc.outlineB = 0.08;
    mc.dockFillR = 0.12; mc.dockFillG = 0.12; mc.dockFillB = 0.14;
    mc.drawerDimR = 0.11; mc.drawerDimG = 0.11; mc.drawerDimB = 0.13;
    mc.notifCriticalBgR = 0.18; mc.notifCriticalBgG = 0.06; mc.notifCriticalBgB = 0.06;
    mc.notifCriticalOutlineR = 0.70; mc.notifCriticalOutlineG = 0.10; mc.notifCriticalOutlineB = 0.10;
  }
  return mc;
}

// ── icon theme ───────────────────────────────────────────────────────

std::string read_dock_icon_theme_from_disk() {
  std::string path = state_settings_toml_path();
  std::ifstream f(path);
  if (!f.is_open()) return {};
  try {
    toml::table tbl = toml::parse(f);
    if (auto* dock = tbl["dock"].as_table()) {
      if (auto* v = dock->get("iconTheme")) return v->value_or("");
    }
  } catch (...) {}
  return {};
}

// ── File Browser settings ────────────────────────────────────────────

FileBrowserSettings read_file_browser_toml() {
  FileBrowserSettings fbs;
  std::string path = file_browser_toml_path();
  std::ifstream f(path);
  if (!f.is_open()) return fbs;

  try {
    toml::table tbl = toml::parse(f);
    fbs.zoom_pct = tbl["zoom_pct"].value_or(100.0);
    fbs.folders_before_files = tbl["folders_before_files"].value_or(true);
    fbs.surface_opacity_pct = tbl["surface_opacity_pct"].value_or(100);
    fbs.sidebar_opacity_pct = tbl["sidebar_opacity_pct"].value_or(100);
    fbs.topbar_opacity_pct = tbl["topbar_opacity_pct"].value_or(100);
    fbs.statusbar_opacity_pct = tbl["statusbar_opacity_pct"].value_or(100);
    fbs.preview_opacity_pct = tbl["preview_opacity_pct"].value_or(100);
    fbs.preview_scale = tbl["preview_scale"].value_or(1.0);
    fbs.dialog_opacity_pct = tbl["dialog_opacity_pct"].value_or(100);
    fbs.properties_opacity_pct = tbl["properties_opacity_pct"].value_or(100);
    fbs.view_mode = tbl["view_mode"].value_or(0);
    fbs.sort_field = tbl["sort_field"].value_or(0);
    fbs.sort_descending = tbl["sort_descending"].value_or(false);
    fbs.sort_natural = tbl["sort_natural"].value_or(true);
    fbs.sort_case_sensitive = tbl["sort_case_sensitive"].value_or(false);
    fbs.sort_hidden_last = tbl["sort_hidden_last"].value_or(false);
    fbs.show_hidden = tbl["show_hidden"].value_or(false);
    fbs.filter_bar_open = tbl["filter_bar_open"].value_or(false);
    fbs.window_controls_left = tbl["window_controls_left"].value_or(false);
    fbs.group_by_type = tbl["group_by_type"].value_or(false);
    fbs.group_field = static_cast<int>(tbl["group_field"].value_or(-1));
    fbs.col_owner = tbl["col_owner"].value_or(false);
    fbs.col_group = tbl["col_group"].value_or(false);
    fbs.col_perms = tbl["col_perms"].value_or(false);
    fbs.col_ext = tbl["col_ext"].value_or(false);
    fbs.col_target = tbl["col_target"].value_or(false);
    fbs.dynamic_view = tbl["dynamic_view"].value_or(true);
    fbs.per_folder_props = tbl["per_folder_props"].value_or(false);
    fbs.independent_dir_views = tbl["independent_dir_views"].value_or(false);

    if (auto* fav = tbl["favorites"].as_array()) {
      for (auto& el : *fav) {
        if (auto s = el.value<std::string>()) fbs.favorites.push_back(*s);
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "[horizon-files] TOML parse error (file-browser): " << e.what() << "\n";
  }

  return fbs;
}

bool write_file_browser_toml(const FileBrowserSettings& fbs) {
  try {
    toml::table tbl;
    tbl.emplace("zoom_pct", fbs.zoom_pct);
    tbl.emplace("folders_before_files", fbs.folders_before_files);
    tbl.emplace("surface_opacity_pct", fbs.surface_opacity_pct);
    tbl.emplace("sidebar_opacity_pct", fbs.sidebar_opacity_pct);
    tbl.emplace("topbar_opacity_pct", fbs.topbar_opacity_pct);
    tbl.emplace("statusbar_opacity_pct", fbs.statusbar_opacity_pct);
    tbl.emplace("preview_opacity_pct", fbs.preview_opacity_pct);
    tbl.emplace("preview_scale", fbs.preview_scale);
    tbl.emplace("dialog_opacity_pct", fbs.dialog_opacity_pct);
    tbl.emplace("properties_opacity_pct", fbs.properties_opacity_pct);
    tbl.emplace("view_mode", fbs.view_mode);
    tbl.emplace("sort_field", fbs.sort_field);
    tbl.emplace("sort_descending", fbs.sort_descending);
    tbl.emplace("sort_natural", fbs.sort_natural);
    tbl.emplace("sort_case_sensitive", fbs.sort_case_sensitive);
    tbl.emplace("sort_hidden_last", fbs.sort_hidden_last);
    tbl.emplace("show_hidden", fbs.show_hidden);
    tbl.emplace("filter_bar_open", fbs.filter_bar_open);
    tbl.emplace("window_controls_left", fbs.window_controls_left);
    tbl.emplace("group_by_type", fbs.group_by_type);
    tbl.emplace("group_field", fbs.group_field);
    tbl.emplace("col_owner", fbs.col_owner);
    tbl.emplace("col_group", fbs.col_group);
    tbl.emplace("col_perms", fbs.col_perms);
    tbl.emplace("col_ext", fbs.col_ext);
    tbl.emplace("col_target", fbs.col_target);
    tbl.emplace("dynamic_view", fbs.dynamic_view);
    tbl.emplace("per_folder_props", fbs.per_folder_props);
    tbl.emplace("independent_dir_views", fbs.independent_dir_views);

    toml::array favs;
    for (const auto& f : fbs.favorites) favs.push_back(f);
    tbl.emplace("favorites", std::move(favs));

    std::string path = file_browser_toml_path();
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << tbl << "\n";
    return true;
  } catch (...) { return false; }
}

bool write_state_settings_toml(const ShellConfig& c) {
  try {
    std::string path = state_settings_toml_path();
    fs::create_directories(fs::path(path).parent_path());

    // Parse the existing file and overlay only the keys this app owns, so
    // externally-synced data (color engine matugen*R/G/B floats,
    // matugenPaletteOk, wallpaperImage, ...) survives a settings save.
    toml::table tbl;
    {
      std::ifstream in(path);
      if (in) {
        try {
          tbl = toml::parse(in);
        } catch (const std::exception& e) {
          std::cerr << "[horizon-files] TOML parse error (existing settings): " << e.what() << "\n";
          tbl = toml::table{};
        }
      }
    }

    toml::table ap;
    if (auto* old = tbl["appearance"].as_table()) ap = *old;
    ap.insert_or_assign("matugenThemingEnabled", c.appearance.matugenThemingEnabled);
    ap.insert_or_assign("colorEngineEnabled", c.appearance.colorEngineEnabled);
    ap.insert_or_assign("matugenPaletteOk", c.appearance.matugenPaletteOk);
    ap.insert_or_assign("colorBrightness", c.appearance.colorBrightness);
    ap.insert_or_assign("colorContrast", c.appearance.colorContrast);
    ap.insert_or_assign("colorVibrance", c.appearance.colorVibrance);
    ap.insert_or_assign("colorGamma", c.appearance.colorGamma);
    tbl.insert_or_assign("appearance", std::move(ap));

    toml::table dock;
    if (auto* old = tbl["dock"].as_table()) dock = *old;
    dock.insert_or_assign("iconTheme", c.dock.iconTheme);
    tbl.insert_or_assign("dock", std::move(dock));

    toml::table da;
    if (auto* old = tbl["defaultApps"].as_table()) da = *old;
    da.insert_or_assign("terminal", c.defaultApps.terminal);
    tbl.insert_or_assign("defaultApps", std::move(da));

    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << tbl << "\n";
    return true;
  } catch (...) { return false; }
}

}
