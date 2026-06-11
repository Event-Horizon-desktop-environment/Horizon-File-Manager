#pragma once

namespace eh::file_browser {

/// Run the file browser as a standalone xdg-toplevel window.
/// Creates its own Wayland connection and event loop.
/// Blocks until the window is closed.
/// Returns 0 on success, 1 on error.
[[nodiscard]] int run_standalone();

} // namespace eh::file_browser
