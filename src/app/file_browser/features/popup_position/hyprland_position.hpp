#pragma once

namespace eh::file_browser {

struct WindowRect {
  int x = 0, y = 0, w = 0, h = 0;
};

// Screen-space rectangle of this process's toplevel, from Hyprland's IPC.
bool hyprland_window_rect(WindowRect& out);

// Monitor containing (gx, gy) in screen space, from Hyprland's IPC.
bool hyprland_monitor_for_point(int gx, int gy, WindowRect& out);

}  // namespace eh::file_browser
