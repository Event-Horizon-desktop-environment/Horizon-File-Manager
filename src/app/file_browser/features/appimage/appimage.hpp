// appimage.hpp — AppImage detection, icon extraction and launching.
//
// AppImages are self-contained ELF+squashfs bundles. Two things make them
// feel broken in a file manager that only knows MIME types:
//   1. They all paint with the generic executable icon, even though every
//      AppImage ships its own icon (usually .DirIcon at the squashfs root).
//   2. They are ordinary executables once the executable bit is set; the
//      runtime itself understands --appimage-extract and the FUSE-less
//      APPIMAGE_EXTRACT_AND_RUN fallback.
//
// This module keeps extraction/caching out of the paint path: callers ask
// for a cached icon every frame, while prewarm_tab_icons() asks for the
// extraction once after a directory listing lands.
#pragma once

#include <string>

namespace eh::file_browser {

// True for the conventional .AppImage/.appimage extension (case-insensitive).
bool is_appimage_path(const std::string& path);

// Absolute path of an already-extracted icon for `path`, or {} when it has
// not been extracted yet. Cheap: memoized per path.
std::string appimage_icon_cached(const std::string& path);

// Extract (once) and cache the AppImage's icon, returning the absolute icon
// file path or {} on failure. May spawn the AppImage runtime; never called
// from the paint path.
std::string appimage_icon_path(const std::string& path);

// Make `path` executable if needed and launch it detached. AppImages use the
// runtime's FUSE-less extraction mode when /dev/fuse is unavailable.
bool launch_appimage(const std::string& path);

// Launch an arbitrary executable file detached (AppImage-aware). Returns
// false when the process could not be forked or the file is unusable.
bool launch_program(const std::string& path);

}  // namespace eh::file_browser
