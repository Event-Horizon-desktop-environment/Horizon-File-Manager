// filemanager1.hpp — org.freedesktop.FileManager1 D-Bus service.
//
// Web browsers (Chrome/Firefox "Show in folder") reveal downloads via the
// freedesktop FileManager1 API (ShowItems with file:// URIs), NOT via
// xdg-open/CLI. Without a service owning org.freedesktop.FileManager1 the
// browser falls back to opening just the parent folder, so the file is never
// selected. This module owns that bus name and translates ShowFolders /
// ShowItems into horizon-files window launches (files trigger the existing
// parent+pending_select_path reveal path).
#pragma once

#include <string>
#include <vector>

namespace eh::file_browser {

// Decode a file:// URI (or plain path) to an absolute filesystem path.
// Wraps normalize_launch_path; returns {} for empty/unusable input.
std::string filemanager1_uri_to_path(const std::string& uri);

// Spawn a detached `horizon-files <path>` window for a file-or-dir path.
// Files open their containing folder with the file selected (via the launch
// reveal path); directories open directly. Never blocks or throws.
void filemanager1_open_path(const std::string& path);

// Blocking D-Bus service loop owning org.freedesktop.FileManager1 and serving
// ShowFolders/ShowItems/ShowItemProperties. Runs until the process exits;
// never throws (D-Bus absence is silently ignored so the file manager still
// starts when no session bus exists).
void filemanager1_run_service();

// Best-effort background thread owning the FileManager1 name while this
// process lives (used by normal browser windows so late browser ShowItems
// calls spawn new windows). No-op when the name is already owned.
void filemanager1_start_in_background();

}  // namespace eh::file_browser
