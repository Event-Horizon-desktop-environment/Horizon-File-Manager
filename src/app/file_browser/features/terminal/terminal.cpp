#include "../../app.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "config/shell_config.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "platform/widgets/app_drawer/list/desktop_list.hpp"

namespace xdg = eh::shell::desktop::xdg;

namespace eh::file_browser {

static std::string sh_quote(const std::string& s) {
  std::string q = "'";
  for (char c : s) {
    if (c == '\'') q += "'\\''";
    else q += c;
  }
  q += '\'';
  return q;
}


void scan_terminal_apps(AppState& app) {
  app.term_chooser_apps.clear();
  auto entries = eh::app_drawer::copy_desktop_entries();
  for (const auto& e : entries) {
    if (e.noDisplay || e.hidden) continue;
    bool is_term = false;
    std::string cats_lower = e.categories;
    for (char& ch : cats_lower)
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string cat_check = ";" + cats_lower + ";";
    if (cat_check.find(";terminalemulator;") != std::string::npos)
      is_term = true;
    if (!is_term && e.mimeTypesLower.find("application/x-terminal-emulator") != std::string::npos)
      is_term = true;
    if (!is_term) continue;

    AppState::TerminalApp ta;
    ta.desktop_id = e.path;
    ta.name = e.name.empty() ? e.path : e.name;
    ta.exec = e.exec;
    app.term_chooser_apps.push_back(std::move(ta));
  }
}

//
// Many terminals ignore the inherited working directory and always
// start in $HOME, so an explicit flag is passed. This table records the
// per-terminal flag.

struct TerminalInfo {
  const char* stem;
  const char* flag; // nullptr = inherits CWD, no flag needed
};

static const TerminalInfo kKnownTerminals[] = {
  {"org.kde.konsole",        "--workdir"},
  {"konsole",                "--workdir"},
  {"gnome-terminal",         "--working-directory"},
  {"gnome-terminal.wrapper", "--working-directory"},
  {"xfce4-terminal",         "--working-directory"},
  {"lxterminal",             "--working-directory"},
  {"terminator",             "--working-directory"},
  {"tilix",                  "--working-directory"},
  {"qterminal",              "--workdir"},
  {"kitty",                  "--directory"},
  {"deepin-terminal",        "--workdir"},
  {"cool-retro-term",        "--workdir"},
  // Ptyxis is single-instance (DBusActivatable): a bare `ptyxis` call just
  // activates the existing window and ignores CWD. `--new-window` forces a
  // new window; combined with `--working-directory` per `ptyxis --help` /
  // man page ("may be combined with --working-directory").
  {"ptyxis",                 "--new-window --working-directory"},

  // Terminals that honour inherited CWD — no flag needed.
  {"alacritty",              nullptr},
  {"foot",                   nullptr},
  {"st",                     nullptr},
  {"urxvt",                  nullptr},
  {"rxvt",                   nullptr},
  {"xterm",                  nullptr},
  {"sakura",                 nullptr},
};

static std::string to_lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Basename stem, lowercased, without .desktop suffix:
// "/usr/share/applications/org.gnome.Ptyxis.desktop" -> "org.gnome.ptyxis"
// "ptyxis" -> "ptyxis"
static std::string terminal_stem_lower(const std::string& id) {
  std::string stem = id;
  if (auto slash = stem.rfind('/'); slash != std::string::npos)
    stem = stem.substr(slash + 1);
  std::string low = to_lower_copy(stem);
  const std::string suffix = ".desktop";
  if (low.size() > suffix.size() &&
      low.compare(low.size() - suffix.size(), suffix.size(), suffix) == 0) {
    stem = stem.substr(0, stem.size() - suffix.size());
    low = to_lower_copy(stem);
  }
  return low;
}

static bool terminal_stem_matches(const std::string& stem_low, const std::string& want_low) {
  if (stem_low == want_low) return true;
  // Reverse-domain match: "org.gnome.ptyxis" matches "ptyxis".
  if (stem_low.size() > want_low.size() + 1 &&
      stem_low.compare(stem_low.size() - want_low.size() - 1, want_low.size() + 1,
                       "." + want_low) == 0)
    return true;
  return false;
}

// True when a stored terminal id refers to the given .desktop entry:
// exact path, exact stem (case-insensitive), or bare-vs-domain
// ("ptyxis" vs "org.gnome.Ptyxis").
static bool terminal_id_matches(const std::string& desktop_id, const std::string& term_id) {
  if (desktop_id == term_id) return true;
  std::string a = terminal_stem_lower(desktop_id);
  std::string b = terminal_stem_lower(term_id);
  if (a == b) return true;
  if (terminal_stem_matches(a, b) || terminal_stem_matches(b, a)) return true;
  return false;
}

// GTK >= 4.23.3 binds ext-background-effect on every window with an empty
// blur region, which tells Hyprland >= 0.56 "no blur wanted" (GNOME/gtk#8344).
// Hiding the protocol global from GTK restores compositor auto-blur. Only
// the main Ptyxis process reads it (single-instance), but prefixing every
// launch covers cold starts from any entry point (keybind, .desktop,
// file manager, D-Bus activation).
  // NOTE: this function is deliberately compiled at O0 (see push_options
  // below). With GCC 16 -O3 the "ext_background_effect_manager_v1" bytes
  // came out corrupted in the object ("..._mafect_..."), silently breaking
  // the workaround — verified byte-for-byte in the .o. Splitting the value
  // into pieces was not sufficient on its own; the O0 guard is what holds.
#pragma GCC push_options
#pragma GCC optimize("O0")
static std::string terminal_env_prefix(const std::string& desktop_id) {
  if (!terminal_stem_matches(terminal_stem_lower(desktop_id), "ptyxis"))
    return {};
  // NOTE: keep this value out of a single string literal (see above).
  std::string v = "env GDK_WAYLAND_DISABLE=";
  v += "ext_back";
  v += "ground_effect_manager_v1 ";
  return v;
}
#pragma GCC pop_options

static const char* terminal_workdir_flag(const std::string& desktop_id) {
  const std::string stem_low = terminal_stem_lower(desktop_id);
  // Exact matches first so "gnome-terminal.wrapper" can't be shadowed.
  for (const auto& t : kKnownTerminals) {
    if (stem_low == to_lower_copy(t.stem)) return t.flag;
  }
  // Then reverse-domain suffix matches ("org.gnome.ptyxis" -> "ptyxis").
  for (const auto& t : kKnownTerminals) {
    if (terminal_stem_matches(stem_low, to_lower_copy(t.stem))) return t.flag;
  }
  return nullptr; // unknown → rely on cd
}

// Remove %-placeholders from a desktop-file Exec line.
static std::string clean_exec_line(const std::string& exec_line) {
  std::string cleaned;
  for (size_t j = 0; j < exec_line.size(); ++j) {
    if (exec_line[j] == '%' && j + 1 < exec_line.size()) {
      if (exec_line[j + 1] == '%') {
        cleaned += '%';
        ++j;
      }
      ++j;
      continue;
    }
    cleaned += exec_line[j];
  }
  // Trim leading spaces.
  while (!cleaned.empty() && cleaned[0] == ' ') cleaned.erase(0, 1);
  return cleaned;
}

void open_terminal_at(AppState& app, const std::string& dir) {
  const auto& sc = eh::config::shell_config_snapshot();
  std::string term_id = sc.defaultApps.terminal;

  if (term_id.empty()) {
    scan_terminal_apps(app);
    if (app.term_chooser_apps.empty()) return;
    app.term_chooser_open = true;
    app.term_chooser_target_dir = dir;
    app.term_chooser_hover = -1;
    app.term_chooser_scroll = 0;
    app.term_chooser_w = 400;
    app.term_chooser_x = (app.width - app.term_chooser_w) / 2;
    return;
  }

  scan_terminal_apps(app);
  bool found = false;
  for (const auto& ta : app.term_chooser_apps) {
    if (terminal_id_matches(ta.desktop_id, term_id)) {
      std::string cleaned = clean_exec_line(ta.exec);
      const char* wd_flag = terminal_workdir_flag(ta.desktop_id);

      std::string full;
      const std::string env = terminal_env_prefix(ta.desktop_id);
      if (wd_flag) {
        // Known terminal – pass directory via explicit flag.
        full = env + cleaned + " " + wd_flag + " " + sh_quote(dir) + " >/dev/null 2>&1";
      } else {
        // Unknown terminal / inherits CWD – cd first, then launch.
        full = "cd " + sh_quote(dir) + " && " + env + cleaned + " >/dev/null 2>&1";
      }
      xdg::spawn_sh_lc_detached(full);
      found = true;
      break;
    }
  }
  if (!found) {
    // No .desktop entry matched – try term_id as a raw command name.
    const char* wd_flag = terminal_workdir_flag(term_id);
    const std::string env = terminal_env_prefix(term_id);
    std::string full;
    if (wd_flag) {
      full = env + term_id + " " + wd_flag + " " + sh_quote(dir) + " >/dev/null 2>&1";
    } else {
      full = "cd " + sh_quote(dir) + " && " + env + term_id + " >/dev/null 2>&1";
    }
    xdg::spawn_sh_lc_detached(full);
  }
}

} // namespace eh::file_browser
