// open_with.cpp — Exported from features/menu.cpp as part of the Step 5 file split.

#include "../../app.hpp"
#include "app/file_browser/features/compare/compare.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/dirprops/dirprops.hpp"
#include "app/file_browser/features/progress/progress.hpp"
#include "app/file_browser/features/selection/selection.hpp"
#include "app/file_browser/features/tab_history/tab_history.hpp"
#include "app/file_browser/features/tags/tags.hpp"
#include "app/file_browser/features/view_zoom/view_zoom.hpp"

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

// ── Open With dialog ──────────────────────────────────────────────

static std::string detect_mime_type(const std::string& file_path) {
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

void open_with_open(AppState& app, const std::string& file_path) {
  open_with_close(app);

  const std::string mime_type = detect_mime_type(file_path);
  auto entries = eh::app_drawer::copy_desktop_entries();

  // Build a set of MIME types to search: the exact type, its parent, and wildcard
  std::vector<std::string> mime_variants;
  mime_variants.push_back(mime_type);
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

  app.open_with_open = true;
  app.open_with_file_path = file_path;
  app.open_with_mime = mime_type;
  app.open_with_hover = -1;
  app.open_with_selected = -1;
  app.open_with_scroll = 0;
  app.open_with_set_default = false;
}

void open_with_close(AppState& app) {
  app.open_with_open = false;
  app.open_with_apps.clear();
  app.open_with_file_path.clear();
  app.open_with_mime.clear();
  app.open_with_hover = -1;
  app.open_with_selected = -1;
  app.open_with_scroll = 0;
  app.open_with_exact_count = 0;
  app.open_with_set_default = false;
}

} // namespace eh::file_browser
