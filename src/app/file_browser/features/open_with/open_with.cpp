// open_with.cpp — Exported from features/menu.cpp as part of the Step 5 file split.

#include "../../app.hpp"
#include "app/file_browser/features/appimage/appimage.hpp"
#include "app/file_browser/features/compare/compare.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/dirprops/dirprops.hpp"
#include "app/file_browser/features/progress/progress.hpp"
#include "app/file_browser/features/selection/selection.hpp"
#include "app/file_browser/features/tab_history/tab_history.hpp"
#include "app/file_browser/features/tags/tags.hpp"
#include "app/file_browser/features/view_zoom/view_zoom.hpp"

#include <algorithm>
#include <cctype>
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
#include "platform/common/icon_cache/icon_cache.hpp"
#include "platform/common/palette/matugen_palette.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "dialog/file_chooser_dialog.hpp"
#include "platform/widgets/app_drawer/list/desktop_list.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;
using menu_clock = std::chrono::steady_clock;

namespace eh::file_browser {


static std::string detect_mime_type(const std::string& file_path) {
  // xdg-mime reports AppImages as a generic executable on many systems, which
  // hides every AppImage-aware entry from the Open With list.
  if (is_appimage_path(file_path)) return "application/x-appimage";

  const std::string quoted = "'" + file_path + "'";
  const std::string cmd = "xdg-mime query filetype " + quoted + " 2>/dev/null || "
                          "file -b --mime-type " + quoted + " 2>/dev/null";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return {};
  std::string out;
  char buf[256];
  while (fgets(buf, sizeof(buf), f)) out += buf;
  pclose(f);

  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                          out.back() == ' ' || out.back() == '\t'))
    out.pop_back();
  while (!out.empty() && (out.front() == ' ' || out.front() == '\t'))
    out.erase(out.begin());

  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

static std::vector<std::string> get_mime_associations(const std::string& mime_type) {
  auto read_cache = [&](const std::string& path) -> std::string {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return {};
    std::string result;
    char buf[1024];
    std::string prefix = mime_type + "=";
    while (fgets(buf, sizeof(buf), f)) {
      std::string line = buf;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
      if (line.size() > prefix.size() &&
          line.compare(0, prefix.size(), prefix) == 0) {
        result = line.substr(prefix.size());
        break;
      }
    }
    fclose(f);
    return result;
  };

  auto read_apps = [&](const std::string& path) -> std::string {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return {};
    std::string result;
    char buf[1024];
    std::string prefix = mime_type + "=";
    bool inDefaults = false, inAdded = false;
    while (fgets(buf, sizeof(buf), f)) {
      std::string line = buf;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
      if (line == "[Default Applications]") { inDefaults = true; inAdded = false; continue; }
      if (line == "[Added Associations]") { inAdded = true; inDefaults = false; continue; }
      if (line.empty() || line[0] == '#' || line[0] == '[') {
        if (!line.empty() && line[0] == '[' && line != "[Default Applications]" && line != "[Added Associations]")
          inDefaults = false, inAdded = false;
        continue;
      }
      if ((inDefaults || inAdded) && line.size() > prefix.size() &&
          line.compare(0, prefix.size(), prefix) == 0) {
        if (!result.empty()) result += ';';
        result += line.substr(prefix.size());
      }
    }
    fclose(f);
    return result;
  };

  auto parse_ids = [](const std::string& list) -> std::vector<std::string> {
    std::vector<std::string> ids;
    size_t start = 0;
    while (start < list.size()) {
      size_t semi = list.find(';', start);
      std::string id = (semi == std::string::npos)
          ? list.substr(start) : list.substr(start, semi - start);
      while (!id.empty() && id.front() == ' ') id.erase(id.begin());
      while (!id.empty() && id.back() == ' ') id.pop_back();
      if (!id.empty()) ids.push_back(id);
      if (semi == std::string::npos) break;
      start = semi + 1;
    }
    return ids;
  };

  std::vector<std::string> all;
  auto add = [&](const std::string& list) {
    auto v = parse_ids(list);
    all.insert(all.end(), v.begin(), v.end());
  };

  const char* home = getenv("HOME");
  if (home) {
    std::string ud = std::string(home) + "/.local/share/applications";
    add(read_cache(ud + "/mimeinfo.cache"));
    add(read_apps(ud + "/mimeapps.list"));
    add(read_apps(std::string(home) + "/.config/mimeapps.list"));
  }
  add(read_cache("/usr/share/applications/mimeinfo.cache"));
  add(read_apps("/usr/share/applications/mimeapps.list"));

  return all;
}

// First entry from [Default Applications] for this MIME type, if any.
static std::string read_default_app_id(const std::string& mime_type) {
  if (mime_type.empty()) return {};
  std::vector<std::string> paths;
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  if (xdg && *xdg)
    paths.push_back(std::string(xdg) + "/mimeapps.list");
  else if (home && *home)
    paths.push_back(std::string(home) + "/.config/mimeapps.list");
  if (home && *home) {
    paths.push_back(std::string(home) + "/.local/share/applications/mimeapps.list");
    paths.push_back(std::string(home) + "/.local/share/applications/defaults.list");
  }
  paths.push_back("/usr/share/applications/mimeapps.list");
  paths.push_back("/usr/share/applications/defaults.list");

  const std::string prefix = mime_type + "=";
  for (const auto& path : paths) {
    std::ifstream in(path);
    if (!in) continue;
    std::string line;
    bool in_defaults = false;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line == "[Default Applications]") { in_defaults = true; continue; }
      if (!line.empty() && line[0] == '[') { in_defaults = false; continue; }
      if (!in_defaults || line.compare(0, prefix.size(), prefix) != 0) continue;
      std::string ids = line.substr(prefix.size());
      auto semi = ids.find(';');
      if (semi != std::string::npos) ids.resize(semi);
      while (!ids.empty() && (ids.front() == ' ' || ids.front() == '\t'))
        ids.erase(ids.begin());
      while (!ids.empty() && (ids.back() == ' ' || ids.back() == '\t'))
        ids.pop_back();
      if (!ids.empty()) return ids;
    }
  }
  return {};
}

void open_with_apply_filter(AppState& app) {
  app.open_with_visible.clear();
  std::string query = app.open_with_query;
  for (char& c : query)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  for (int i = 0; i < static_cast<int>(app.open_with_apps.size()); ++i) {
    if (query.empty()) {
      app.open_with_visible.push_back(i);
      continue;
    }
    std::string name = app.open_with_apps[i].name;
    std::string id = app.open_with_apps[i].desktop_id;
    for (char& c : name)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : id)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name.find(query) != std::string::npos ||
        id.find(query) != std::string::npos)
      app.open_with_visible.push_back(i);
  }

  app.open_with_visible_exact_count = 0;
  for (int idx : app.open_with_visible) {
    if (idx < app.open_with_exact_count)
      ++app.open_with_visible_exact_count;
    else
      break;
  }

  const bool selected_visible =
      app.open_with_selected >= 0 &&
      std::find(app.open_with_visible.begin(), app.open_with_visible.end(),
                app.open_with_selected) != app.open_with_visible.end();
  if (!selected_visible)
    app.open_with_selected =
        app.open_with_visible.empty() ? -1 : app.open_with_visible.front();

  if (app.open_with_hover >= 0 &&
      std::find(app.open_with_visible.begin(), app.open_with_visible.end(),
                app.open_with_hover) == app.open_with_visible.end())
    app.open_with_hover = -1;
}

void open_with_set_default_app(const std::string& mime_type,
                               const std::string& desktop_id_raw) {
  if (mime_type.empty() || desktop_id_raw.empty()) return;

  std::string desktop_id = desktop_id_raw;
  if (desktop_id.size() < 8 ||
      desktop_id.compare(desktop_id.size() - 8, 8, ".desktop") != 0)
    desktop_id += ".desktop";

  const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  std::string config_dir;
  if (xdg_config && *xdg_config)
    config_dir = xdg_config;
  else if (home && *home)
    config_dir = std::string(home) + "/.config";
  if (config_dir.empty()) return;

  std::error_code ec;
  fs::create_directories(config_dir, ec);
  const fs::path path = fs::path(config_dir) / "mimeapps.list";

  std::vector<std::string> mime_types{mime_type};
  if (mime_type == "application/x-appimage" ||
      mime_type == "application/vnd.appimage") {
    mime_types = {"application/x-appimage", "application/vnd.appimage",
                  "application/x-executable"};
  }

  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }

  std::vector<std::string> out;
  bool in_defaults = false;
  size_t defaults_pos = std::string::npos;
  for (const auto& line : lines) {
    if (line == "[Default Applications]") {
      in_defaults = true;
      out.push_back(line);
      defaults_pos = out.size() - 1;
      continue;
    }
    if (!line.empty() && line[0] == '[') in_defaults = false;
    if (in_defaults) {
      bool replaces = false;
      for (const auto& mt : mime_types) {
        if (line.rfind(mt + "=", 0) == 0) { replaces = true; break; }
      }
      if (replaces) continue;
    }
    out.push_back(line);
  }

  if (defaults_pos == std::string::npos) {
    if (!out.empty() && !out.back().empty()) out.push_back("");
    out.push_back("[Default Applications]");
    defaults_pos = out.size() - 1;
  }

  std::vector<std::string> additions;
  for (const auto& mt : mime_types)
    additions.push_back(mt + "=" + desktop_id + ";");
  out.insert(out.begin() + static_cast<std::ptrdiff_t>(defaults_pos + 1),
             additions.begin(), additions.end());

  const fs::path tmp = path.string() + ".tmp." + std::to_string(::getpid());
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return;
    for (const auto& line : out) f << line << '\n';
    if (!f) { f.close(); fs::remove(tmp, ec); return; }
  }
  fs::rename(tmp, path, ec);
  if (ec) fs::remove(tmp, ec);
}

void open_with_launch(AppState& app, int app_index) {
  if (app_index < 0 ||
      app_index >= static_cast<int>(app.open_with_apps.size()))
    return;
  const auto& entry = app.open_with_apps[app_index];
  if (app.open_with_set_default)
    open_with_set_default_app(app.open_with_mime, entry.desktop_id);

  std::string desktop = entry.desktop_path;
  std::string file = app.open_with_file_path;
  for (size_t p = 0; (p = desktop.find('\'', p)) != std::string::npos; p += 4)
    desktop.replace(p, 1, "'\\''");
  for (size_t p = 0; (p = file.find('\'', p)) != std::string::npos; p += 4)
    file.replace(p, 1, "'\\''");
  std::string cmd = "gio launch '" + desktop + "' '" + file + "' &";
  (void)std::system(cmd.c_str());
  open_with_close(app);
}

void open_with_open(AppState& app, const std::string& file_path) {
  open_with_close(app);

  const std::string mime_type = detect_mime_type(file_path);
  auto entries = eh::app_drawer::copy_desktop_entries();

  // Build a set of MIME types to search: the exact type, its parent, and wildcard
  std::vector<std::string> mime_variants;
  mime_variants.push_back(mime_type);
  if (is_appimage_path(file_path)) {
    // Different desktops/tools use different AppImage MIME spellings.
    mime_variants.push_back("application/x-appimage");
    mime_variants.push_back("application/vnd.appimage");
    mime_variants.push_back("application/x-executable");
    mime_variants.push_back("application/vnd.appimage+elf");
  }
  {
    // "text/x-diff" → "text/plain" (subtype parent)
    auto slash = mime_type.find('/');
    if (slash != std::string::npos) {
      std::string top = mime_type.substr(0, slash);
      mime_variants.push_back(top + "/*");
      mime_variants.push_back(top + "/plain");
      // "text/x-diff" → "text" (bare top-level)
      mime_variants.push_back(top);
    }
  }

  std::unordered_set<std::string> rec_set;

  // Look up MIME associations for each variant
  for (const auto& mv : mime_variants) {
    auto ids = get_mime_associations(mv);
    for (const auto& id : ids) rec_set.insert(id);
  }

  // Also scan desktop file MimeType fields — check exact, wildcard, and parent
  // Split desktop MimeType string into individual types
  auto split_semi = [](const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
      size_t semi = s.find(';', start);
      std::string token = (semi == std::string::npos) ? s.substr(start) : s.substr(start, semi - start);
      while (!token.empty() && token.front() == ' ') token.erase(token.begin());
      while (!token.empty() && token.back() == ' ') token.pop_back();
      if (!token.empty()) out.push_back(std::move(token));
      if (semi == std::string::npos) break;
      start = semi + 1;
    }
    return out;
  };

  for (const auto& ent : entries) {
    if (ent.noDisplay || ent.hidden) continue;
    if (ent.mimeTypesLower.empty()) continue;

    auto app_types = split_semi(ent.mimeTypesLower);
    bool matched = false;

    for (const auto& mv : mime_variants) {
      if (mv.empty()) continue;
      for (const auto& at : app_types) {
        // Exact match
        if (at == mv) { matched = true; break; }
        // Desktop file declares wildcard like "text/*" — check if mv starts with "text/"
        if (at.size() >= 2 && at[at.size() - 1] == '*' && at[at.size() - 2] == '/') {
          std::string prefix = at.substr(0, at.size() - 1);
          if (mv.size() >= prefix.size() && mv.compare(0, prefix.size(), prefix) == 0) {
            matched = true; break;
          }
        }
      }
      if (matched) break;
    }
    if (matched) {
      std::string id = fs::path(ent.path).stem().string();
      rec_set.insert(id);
      rec_set.insert(id + ".desktop");
    }
  }

  // For text/* MIME types, also recommend apps that identify as text editors
  // via GenericName or Categories (catches apps like VS Code that don't
  // declare text/plain in their MimeType field).
  if (!mime_type.empty() && mime_type.compare(0, 5, "text/") == 0) {
    for (const auto& ent : entries) {
      if (ent.noDisplay || ent.hidden) continue;
      std::string id = fs::path(ent.path).stem().string();
      if (rec_set.count(id) || rec_set.count(id + ".desktop")) continue;
      bool is_text_editor = false;
      // Check GenericName for text/code editor specifically
      {
        std::string gn = ent.genericName;
        for (auto& c : gn) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (gn.find("text editor") != std::string::npos ||
            gn.find("code editor") != std::string::npos ||
            gn.find("source code") != std::string::npos)
          is_text_editor = true;
      }
      // Check Categories for TextEditor
      {
        std::string cats = ent.categories;
        for (auto& c : cats) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (cats.find("texteditor") != std::string::npos) is_text_editor = true;
      }
      if (is_text_editor) {
        rec_set.insert(id);
        rec_set.insert(id + ".desktop");
      }
    }
  }

  std::vector<AppState::OpenWithEntry> recommended, other;
  for (const auto& ent : entries) {
    if (ent.noDisplay || ent.hidden) continue;
    AppState::OpenWithEntry ae;
    ae.desktop_id = fs::path(ent.path).stem().string();
    ae.desktop_path = ent.path;
    ae.name = ent.name;
    ae.icon = ent.icon;
    if (rec_set.count(ae.desktop_id) || rec_set.count(ae.desktop_id + ".desktop"))
      recommended.push_back(std::move(ae));
    else
      other.push_back(std::move(ae));
  }

  app.open_with_exact_count = static_cast<int>(recommended.size());
  app.open_with_apps = std::move(recommended);
  app.open_with_apps.insert(app.open_with_apps.end(),
                             std::make_move_iterator(other.begin()),
                             std::make_move_iterator(other.end()));

  // Prefetch every row icon up front at the size the dialog actually paints
  // (48px bucket for a 34px tile). Without this, offscreen rows would only
  // decode when scrolled into view, which shows up as icons popping in while
  // scrolling. Enqueue only — decoding happens on the icon-cache worker, so
  // opening the dialog never blocks the UI thread.
  for (const auto& entry : app.open_with_apps) {
    if (!entry.icon.empty())
      app.icons.app_icon(entry.icon, 48);
    if (!entry.desktop_id.empty() && entry.desktop_id != entry.icon)
      app.icons.app_icon(entry.desktop_id, 48);
  }
  // Keep repainting while the worker drains so icons pop in promptly.
  app.icon_catchup_frames = 120;

  app.open_with_open = true;
  app.open_with_file_path = file_path;
  app.open_with_mime = mime_type;
  app.open_with_query.clear();
  app.open_with_default_id = read_default_app_id(mime_type);
  app.open_with_hover = -1;
  app.open_with_selected = -1;
  app.open_with_scroll = 0;
  app.open_with_scroll_smooth_current = 0.0;
  app.open_with_scroll_smooth_target = 0.0;
  app.open_with_set_default = false;
  open_with_apply_filter(app);
}

void open_with_close(AppState& app) {
  app.open_with_open = false;
  app.open_with_apps.clear();
  app.open_with_visible.clear();
  app.open_with_file_path.clear();
  app.open_with_mime.clear();
  app.open_with_query.clear();
  app.open_with_default_id.clear();
  app.open_with_hover = -1;
  app.open_with_selected = -1;
  app.open_with_scroll = 0;
  app.open_with_scroll_smooth_current = 0.0;
  app.open_with_scroll_smooth_target = 0.0;
  app.open_with_exact_count = 0;
  app.open_with_visible_exact_count = 0;
  app.open_with_set_default = false;
}

} // namespace eh::file_browser
