// launch_path.hpp — normalize the single "open this location" argument a
// launcher (browser "show in folder", portal, xdg-open, CLI) hands us.
//
// Browsers typically pass either the containing directory or the downloaded
// file itself (Nautilus-style `--select <file>`, or a file:// URI). The old
// code only understood directories: a file path hit navigate_to's
// is_directory check and silently showed an empty Home, which reads as "the
// file manager never opened my folder".
#pragma once

#include <string>

namespace eh::file_browser {

// Trim wrapping whitespace/CR/LF, unwrap file:// URIs (including the
// file://localhost/ form) and percent-decode them. Plain paths pass through
// untouched. Returns {} for empty input.
std::string normalize_launch_path(const std::string& in);

// Split a normalized-or-raw launch target into the directory to open plus an
// optional file to select once its listing lands. Returns false when neither
// the path nor its parent is an existing directory.
bool split_launch_target(const std::string& in, std::string& out_dir,
                         std::string& out_select);

// First usable CLI launch target: honors `--select <path>` / `--select=<path>`
// (Nautilus-style "reveal this file"), otherwise the first non-flag argument.
// Returns {} when there is nothing to navigate to.
std::string find_cli_launch_target(int argc, char** argv);

}  // namespace eh::file_browser
