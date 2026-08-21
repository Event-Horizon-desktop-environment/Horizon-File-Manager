#pragma once

#include "app/file_browser/app_types.hpp"

namespace eh::file_browser {

// True when one of the supported diff tools (meld, kompare, diffuse) is
// installed and reachable through $PATH.
bool compare_tool_available();

// Name of the first available diff tool, or an empty string.
const char* compare_tool_name();

// Launch the diff tool with exactly two selected entries. Shows a status
// toast when the selection is not exactly two files.
void compare_selected_files(AppState& app);

} // namespace eh::file_browser
