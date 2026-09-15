// settings.cpp — Exported from features/menu.cpp as part of the Step 5 file split.

#include "../app.hpp"
#include "app/file_browser/features/compare.hpp"
#include "app/file_browser/features/compress.hpp"
#include "app/file_browser/features/dirprops.hpp"
#include "app/file_browser/features/progress.hpp"
#include "app/file_browser/features/selection.hpp"
#include "app/file_browser/features/tab_history.hpp"
#include "app/file_browser/features/tags.hpp"
#include "app/file_browser/features/view_zoom.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config/shell_config.hpp"
#include "base/thread/thread_dispatch.hpp"
#include "platform/common/palette/matugen_palette.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "dialog/file_chooser_dialog.hpp"
#include "platform/widgets/app_drawer/list/desktop_list.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;
using menu_clock = std::chrono::steady_clock;

namespace eh::file_browser {

// ── Settings dialog open / apply ──────────────────────────────────

void open_settings(AppState& app) {
  // File browser settings come from the live app state (already loaded
  // by reload_settings_from_config).  Terminal choice is a global setting
  // stored in the main settings.toml.
  app.settings_zoom_pct = app.zoom_pct;
  app.settings_folders_before_files = app.folders_before_files;
  app.settings_independent_dir_views = app.independent_dir_views;
  app.settings_opacity_pct = app.surface_opacity_pct;
  app.settings_sidebar_opacity_pct = app.sidebar_opacity_pct;
  app.settings_topbar_opacity_pct = app.topbar_opacity_pct;
  app.settings_statusbar_opacity_pct = app.statusbar_opacity_pct;
  app.settings_preview_opacity_pct = app.preview_opacity_pct;
  app.settings_dialog_opacity_pct = app.dialog_opacity_pct;
  app.settings_properties_opacity_pct = app.properties_opacity_pct;

  {
    const auto& sc = eh::config::shell_config_snapshot();
    app.settings_matugen_theming = sc.appearance.matugenThemingEnabled;
    app.settings_color_engine = sc.appearance.colorEngineEnabled;
  }

  // Build terminal options list
  scan_terminal_apps(app);
  app.settings_term_opts.clear();
  app.settings_term_opts.push_back("System default");
  int default_idx = 0;
  {
    const auto& sc = eh::config::shell_config_snapshot();
    for (size_t i = 0; i < app.term_chooser_apps.size(); ++i) {
      app.settings_term_opts.push_back(app.term_chooser_apps[i].name);
      std::string id = app.term_chooser_apps[i].desktop_id;
      auto dot = id.rfind('.');
      if (dot != std::string::npos) id = id.substr(0, dot);
      auto slash = id.rfind('/');
      if (slash != std::string::npos) id = id.substr(slash + 1);
      if (!sc.defaultApps.terminal.empty() &&
          (id == sc.defaultApps.terminal ||
           app.term_chooser_apps[i].desktop_id == sc.defaultApps.terminal ||
           app.term_chooser_apps[i].desktop_id == sc.defaultApps.terminal + ".desktop")) {
        default_idx = static_cast<int>(i) + 1;
      }
    }
  }
  app.settings_default_term_idx = default_idx;

  app.settings_open = true;
  app.settings_tab = 0;
  app.settings_dropdown_open = false;
  app.settings_dropdown_hover = -1;
  app.settings_dropdown_scroll = 0;

  create_settings_window(app);
}

// Bridge the session per-directory state map into the persisted config struct.
static void dir_views_to_config(eh::config::FileBrowserSettings& fbs,
                                const AppState& app) {
  fbs.dir_views.clear();
  fbs.dir_views.reserve(app.dir_view_states.size() + 1);
  for (const auto& [path, st] : app.dir_view_states) {
    eh::config::FileBrowserDirView dv;
    dv.view_mode = static_cast<int>(st.view_mode);
    dv.sort_field = static_cast<int>(st.sort_field);
    dv.sort_descending = st.sort_descending;
    dv.group_by_type = st.group_by_type;
    dv.group_field = st.group_field;
    dv.zoom_level = st.zoom_level;
    fbs.dir_views.emplace(path, dv);
  }
  // The folder we're sitting in is only snapshotted when we navigate AWAY
  // (remember_independent_view), which never happens for the last folder we
  // visit — so capture it live here or its zoom would never reach the disk.
  if (app.independent_dir_views) {
    const std::string& cur = app.cur_tab().current_path;
    if (!cur.empty() && cur != "computer://" && cur != "trash://" &&
        cur.rfind("recent://", 0) != 0) {
      const auto& t = app.cur_tab();
      eh::config::FileBrowserDirView dv;
      dv.view_mode = static_cast<int>(t.view_mode);
      dv.sort_field = static_cast<int>(t.sort_field);
      dv.sort_descending = t.sort_descending;
      dv.group_by_type = t.group_by_type;
      dv.group_field = t.group_field;
      dv.zoom_level = zoom_level_for_pct(app.zoom_pct);
      fbs.dir_views[cur] = dv;
    }
  }
}

// Seed the session per-directory state map from the persisted config struct.
static void dir_views_from_config(AppState& app,
                                  const eh::config::FileBrowserSettings& fbs) {
  app.dir_view_states.clear();
  app.dir_view_states.reserve(fbs.dir_views.size());
  for (const auto& [path, dv] : fbs.dir_views) {
    AppState::DirViewState st;
    st.view_mode = static_cast<ViewMode>(dv.view_mode);
    st.sort_field = static_cast<SortField>(dv.sort_field);
    st.sort_descending = dv.sort_descending;
    st.group_by_type = dv.group_by_type;
    st.group_field = dv.group_field;
    st.zoom_level = dv.zoom_level;
    app.dir_view_states.emplace(path, st);
  }
}

void save_file_browser_settings(AppState& app) {
  eh::config::FileBrowserSettings fbs;
  fbs.zoom_pct = app.zoom_pct;
  fbs.folders_before_files = app.folders_before_files;
  fbs.sort_natural = app.sort_natural;
  fbs.sort_case_sensitive = app.sort_case_sensitive;
  fbs.sort_hidden_last = app.sort_hidden_last;
  fbs.filter_bar_open = app.filter_bar_open_by_default;
  fbs.surface_opacity_pct = app.surface_opacity_pct;
  fbs.sidebar_opacity_pct = app.sidebar_opacity_pct;
  fbs.topbar_opacity_pct = app.topbar_opacity_pct;
  fbs.statusbar_opacity_pct = app.statusbar_opacity_pct;
  fbs.preview_opacity_pct = app.preview_opacity_pct;
  fbs.preview_scale = app.preview_scale;
  fbs.dialog_opacity_pct = app.dialog_opacity_pct;
  fbs.properties_opacity_pct = app.properties_opacity_pct;
  fbs.view_mode = static_cast<int>(app.cur_tab().view_mode);
  fbs.sort_field = static_cast<int>(app.cur_tab().sort_field);
  fbs.sort_descending = app.cur_tab().sort_descending;
  fbs.group_by_type = app.cur_tab().group_by_type;
  fbs.group_field = app.cur_tab().group_field;
  fbs.col_owner = app.col_owner;
  fbs.col_group = app.col_group;
  fbs.col_perms = app.col_perms;
  fbs.col_ext = app.col_ext;
  fbs.col_target = app.col_target;
  fbs.dynamic_view = app.dynamic_view;
  fbs.per_folder_props = app.per_folder_props;
  fbs.independent_dir_views = app.independent_dir_views;
  fbs.show_hidden = app.show_hidden;
  fbs.favorites = app.favorites;
  fbs.window_controls_left = app.window_controls_left;
  dir_views_to_config(fbs, app);
  (void)eh::config::write_file_browser_toml(fbs);
  reload_settings_from_config(app);
}

void settings_apply(AppState& app) {
  // Save file browser settings to its own file
  eh::config::FileBrowserSettings fbs;
  fbs.zoom_pct = app.settings_zoom_pct;
  fbs.folders_before_files = app.settings_folders_before_files;
  fbs.sort_natural = app.sort_natural;
  fbs.sort_case_sensitive = app.sort_case_sensitive;
  fbs.sort_hidden_last = app.sort_hidden_last;
  fbs.filter_bar_open = app.filter_bar_open_by_default;
  fbs.surface_opacity_pct = app.settings_opacity_pct;
  fbs.sidebar_opacity_pct = app.settings_sidebar_opacity_pct;
  fbs.topbar_opacity_pct = app.settings_topbar_opacity_pct;
  fbs.statusbar_opacity_pct = app.settings_statusbar_opacity_pct;
  fbs.preview_opacity_pct = app.settings_preview_opacity_pct;
  fbs.preview_scale = app.settings_preview_scale;
  fbs.dialog_opacity_pct = app.settings_dialog_opacity_pct;
  fbs.properties_opacity_pct = app.settings_properties_opacity_pct;
  fbs.view_mode = static_cast<int>(app.cur_tab().view_mode);
  fbs.sort_field = static_cast<int>(app.cur_tab().sort_field);
  fbs.sort_descending = app.cur_tab().sort_descending;
  fbs.group_by_type = app.cur_tab().group_by_type;
  fbs.group_field = app.cur_tab().group_field;
  fbs.col_owner = app.col_owner;
  fbs.col_group = app.col_group;
  fbs.col_perms = app.col_perms;
  fbs.col_ext = app.col_ext;
  fbs.col_target = app.col_target;
  fbs.show_hidden = app.show_hidden;
  fbs.favorites = app.favorites;
  fbs.independent_dir_views = app.settings_independent_dir_views;
  dir_views_to_config(fbs, app);
  (void)eh::config::write_file_browser_toml(fbs);

  // Terminal preference is a global setting — update the main config
  {
    eh::config::ShellConfig sc = eh::config::shell_config_snapshot();
    if (app.settings_default_term_idx > 0 &&
        app.settings_default_term_idx - 1 < static_cast<int>(app.term_chooser_apps.size())) {
      sc.defaultApps.terminal = app.term_chooser_apps[app.settings_default_term_idx - 1].desktop_id;
    } else {
      sc.defaultApps.terminal.clear();
    }
    sc.appearance.matugenThemingEnabled = app.settings_matugen_theming;
    sc.appearance.colorEngineEnabled = app.settings_color_engine;
    (void)eh::config::write_state_settings_toml(sc);
    eh::config::shell_config_apply_from_memory(std::move(sc));
  }

  reload_settings_from_config(app);
}

void reload_colors_from_config(AppState& app) {
  eh::config::shell_config_reload_from_disk_now();
  const auto& sc = eh::config::shell_config_snapshot_skip_matugen();
  eh::config::ShellAppearance ap = sc.appearance;
  if (ap.colorEngineEnabled) {
    // The rendered horizon-files-matugen.conf is the color engine's direct
    // output — prefer it; on failure keep whatever synced TOML floats parsed.
    (void)eh::matugen::read_shell_color_config(ap);
  } else if (ap.matugenThemingEnabled) {
    eh::matugen::refresh_wallpaper_derived_palette(ap, sc.wallpaperImage);
  } else {
    ap.matugenPaletteOk = false;
  }
  const auto mc = eh::config::derived_chrome_colors(ap);
  if ((ap.colorEngineEnabled || ap.matugenThemingEnabled) && ap.matugenPaletteOk) {
    // Use wallpaper-derived colors for all UI with vibrance boost
    // Mix surface/outline/bg with accent color to avoid flat grey M3 tones
    app.bg_r = mc.panelFillR * 0.5 + mc.accentR * 0.015;
    app.bg_g = mc.panelFillG * 0.5 + mc.accentG * 0.015;
    app.bg_b = mc.panelFillB * 0.5 + mc.accentB * 0.015;
    double s_scale = 0.6;
    app.surface_r = mc.panelFillR * s_scale + mc.accentR * 0.015;
    app.surface_g = mc.panelFillG * s_scale + mc.accentG * 0.015;
    app.surface_b = mc.panelFillB * s_scale + mc.accentB * 0.015;
    app.accent_r = mc.accentR;
    app.accent_g = mc.accentG;
    app.accent_b = mc.accentB;
    double o_blend = 0.6;
    app.outline_r = mc.outlineR * o_blend + mc.accentR * (1.0 - o_blend);
    app.outline_g = mc.outlineG * o_blend + mc.accentG * (1.0 - o_blend);
    app.outline_b = mc.outlineB * o_blend + mc.accentB * (1.0 - o_blend);
    app.text_r = mc.textR;
    app.text_g = mc.textG;
    app.text_b = mc.textB;
    app.text_secondary_r = mc.textR * 0.65;
    app.text_secondary_g = mc.textG * 0.65;
    app.text_secondary_b = mc.textB * 0.65;
  } else {
    app.bg_r = 0.0;
    app.bg_g = 0.0;
    app.bg_b = 0.0;
    app.surface_r = 0.08;
    app.surface_g = 0.08;
    app.surface_b = 0.10;
    eh::config::apply_color_adjustment(app.surface_r, app.surface_g, app.surface_b,
                                       ap.colorBrightness, ap.colorContrast, ap.colorVibrance, ap.colorGamma);
    app.accent_r = mc.accentR;
    app.accent_g = mc.accentG;
    app.accent_b = mc.accentB;
    app.outline_r = 0.06;
    app.outline_g = 0.06;
    app.outline_b = 0.08;
    eh::config::apply_color_adjustment(app.outline_r, app.outline_g, app.outline_b,
                                       ap.colorBrightness, ap.colorContrast, ap.colorVibrance, ap.colorGamma);
    app.text_r = mc.textR;
    app.text_g = mc.textG;
    app.text_b = mc.textB;
    app.text_secondary_r = 0.50;
    app.text_secondary_g = 0.50;
    app.text_secondary_b = 0.55;
  }
}

void reload_settings_from_config(AppState& app) {
  reload_colors_from_config(app);
  eh::config::FileBrowserSettings fbs = eh::config::read_file_browser_toml();
  const auto& sc = eh::config::shell_config_snapshot_skip_matugen();
  app.settings_matugen_theming = sc.appearance.matugenThemingEnabled;
  app.settings_color_engine = sc.appearance.colorEngineEnabled;
  apply_zoom_pct(app, fbs.zoom_pct);
  app.folders_before_files = fbs.folders_before_files;
  app.surface_opacity_pct = fbs.surface_opacity_pct;
  app.sidebar_opacity_pct = fbs.sidebar_opacity_pct;
  app.topbar_opacity_pct = fbs.topbar_opacity_pct;
  app.statusbar_opacity_pct = fbs.statusbar_opacity_pct;
  app.preview_opacity_pct = fbs.preview_opacity_pct;
  app.preview_scale = fbs.preview_scale;
  app.settings_preview_scale = fbs.preview_scale;
  app.dialog_opacity_pct = fbs.dialog_opacity_pct;
  app.properties_opacity_pct = fbs.properties_opacity_pct;
  app.cur_tab().view_mode = static_cast<ViewMode>(fbs.view_mode);
  if (app.cur_tab().view_mode != ViewMode::Computer)
    app.last_browser_view_mode = app.cur_tab().view_mode;
  app.cur_tab().sort_field = static_cast<SortField>(std::clamp(
      fbs.sort_field, 0, static_cast<int>(SortField::LinkTarget)));
  app.cur_tab().sort_descending = fbs.sort_descending;
  app.cur_tab().group_by_type = fbs.group_by_type;
  app.cur_tab().group_field =
      (fbs.group_field >= 0 && fbs.group_field <= 4)
          ? fbs.group_field
          : (fbs.group_by_type ? 1 : 0);
  app.col_owner = fbs.col_owner;
  app.col_group = fbs.col_group;
  app.col_perms = fbs.col_perms;
  app.col_ext = fbs.col_ext;
  app.col_target = fbs.col_target;
  app.dynamic_view = fbs.dynamic_view;
  app.per_folder_props = fbs.per_folder_props;
  app.independent_dir_views = fbs.independent_dir_views;
  dir_views_from_config(app, fbs);
  app.show_hidden = fbs.show_hidden;
  app.sort_natural = fbs.sort_natural;
  app.sort_case_sensitive = fbs.sort_case_sensitive;
  app.sort_hidden_last = fbs.sort_hidden_last;
  app.filter_bar_open_by_default = fbs.filter_bar_open;
  if (fbs.filter_bar_open && !app.filter_bar_default_applied) {
    app.filter_bar_default_applied = true;
    app.search_active = true;
  }
  app.entry_height = std::max(20, static_cast<int>(36.0 * app.zoom_pct / 100.0));
  int icon_sz = static_cast<int>(48.0 * app.zoom_pct / 100.0);
  app.grid_cell_size = std::max(40, icon_sz + static_cast<int>(8.0 * app.zoom_pct / 100.0));
  app.sidebar_width = std::max(120, static_cast<int>(app.sidebar_width_base * app.zoom_pct / 100.0));

  app.favorites = fbs.favorites;
  app.window_controls_left = fbs.window_controls_left;

  // Sync icon theme from shell config
  if (!sc.dock.iconTheme.empty() && sc.dock.iconTheme != app.last_icon_theme) {
    app.icons.set_icon_theme(sc.dock.iconTheme);
    app.last_icon_theme = sc.dock.iconTheme;
    // Theme switch wipes the icon cache; re-resolve the current folder's
    // icons synchronously so the next paint is already final artwork.
    prewarm_tab_icons(app);
  }
}

} // namespace eh::file_browser
