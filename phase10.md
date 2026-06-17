# Phase 10 — Removable Media & Network

## Status: Design Complete · Est. 3,500–5,000 LOC · 8 files new · 12 files modified

---

## Table of Contents
1. [Architecture Map (all features, all files, all touch points)](#1)
2. [Feature 1: ISO/Disk Image Mounting](#2)
3. [Feature 2: Format Drive Dialog](#3)
4. [Feature 3: GVfs Network Mount Backend](#4)
5. [Feature 4: Connect to Server Dialog](#5)
6. [Feature 5: LAN Network Browsing (Discovery)](#6)
7. [Feature 6: Cloud Storage (rclone)](#7)
8. [Build System](#8)
9. [Implementation Order & Dependencies](#9)
10. [Test Strategy](#10)
11. [Risks & Edge Cases](#11)

---

<a name="1"></a>
## 1. Architecture Map

### 1.1 File Inventory

| File | Status | Role | Touches |
|------|--------|------|---------|
| `src/app/file_browser/app_types.hpp` | **MODIFY** | AppState fields, enums, structs for all 6 features | Lines 78–103 (SidebarLocation), 112–129 (ComputerItem), 343–381 (ContextMenuAction), ~484–495 (compress fields), ~687 (mount_poll_wake) |
| `src/app/file_browser/features/nav.cpp` | **MODIFY** | `refresh_sidebar()` — 5 sections, `mount_drive()`/`unmount_drive()` pattern | Lines 939–1103 (sidebar refresh), 1105–1149 (mount/unmount) |
| `src/app/file_browser/features/menu.cpp` | **MODIFY** | `open_context_menu()` + `execute_context_menu_action()` — 12 new actions | Lines 40–161 (builder), 165–895 (dispatch) |
| `src/app/file_browser/embed/embed.cpp` | **MODIFY** | Main loop poll — pick up mount/format/unmount results | Lines 468–547 (mount polling) |
| `src/app/file_browser/ui/draw.cpp` | **MODIFY** | Sidebar draw (5 sections), 4 new dialogs (compass pattern) | Lines 413–747 (sidebar), 2990–3175 (compress dialog) |
| `src/app/file_browser/input/events.cpp` | **MODIFY** | Click/hover/routing for sidebar + 4 dialogs | Lines 622–644 (compress clicks), 1679–1731 (sidebar clicks), 1805–1852 (computer clicks), 2042–2074 (sidebar right-click), 2849–2863 (hover) |
| `src/app/file_browser/ui/computer_view.cpp` | **MODIFY** | `refresh_computer()` + `draw_computer_view()` — Network + Cloud sections | Lines 54–274 (refresh), 278–557 (draw), 561–637 (hit test) |
| `src/config/shell_config.hpp` | **MODIFY** | `FileBrowserSettings` — network bookmarks, cloud remotes, tags | Lines 274–302 |
| `meson.build` | **MODIFY** | Add 8 new .cpp files | Lines 63–137 |
| `src/app/file_browser/features/iso_mounter.hpp` | **NEW** | ISO mount/unmount async API | — |
| `src/app/file_browser/features/iso_mounter.cpp` | **NEW** | `losetup` + `udisksctl` backend | — |
| `src/app/file_browser/features/format_drive_dialog.hpp` | **NEW** | Format dialog state + API | — |
| `src/app/file_browser/features/format_drive_dialog.cpp` | **NEW** | `mkfs.*` backend thread | — |
| `src/app/file_browser/features/network_mounter.hpp` | **NEW** | `gio mount` async API | — |
| `src/app/file_browser/features/network_mounter.cpp` | **NEW** | GVfs mount/unmount + FUSE path detection | — |
| `src/app/file_browser/features/connect_dialog.hpp` | **NEW** | Connect dialog state + render helpers | — |
| `src/app/file_browser/features/connect_dialog.cpp` | **NEW** | Dialog click routing + draw (shared with draw.cpp) | — |
| `src/app/file_browser/features/lan_discovery.hpp` | **NEW** | LAN discovery API | — |
| `src/app/file_browser/features/lan_discovery.cpp` | **NEW** | Avahi/WS-Discovery/Samba backends | — |
| `src/app/file_browser/features/cloud_mounter.hpp` | **NEW** | Cloud state + rclone API | — |
| `src/app/file_browser/features/cloud_mounter.cpp` | **NEW** | rclone FUSE lifecycle | — |

### 1.2 State Machine — Sidebar Location Flow

```
sidebar_locations vector ordering (draw.cpp:419–434):
  [0..places_end-1]        → Places (Home, Desktop, Documents, Downloads, Music, Pictures, Videos, Trash)
  [places_end..fav_start-1]  → Favorites (user-pinned)
  [fav_start..drives_start-1] → (reserved; currently = fav_start for contiguous Drives)
  [drives_start..N-1]        → Drives (Root + UDisks2 block devices + loop)
  NEW [N..N+network_count-1] → Network (GVfs mounts + LAN shares)
  NEW [end..end+cloud_count-1]→ Cloud (rclone FUSE mounts)

Section boundary computation MUST be updated:
  // After existing drives_start computation, add:
  int network_start = drives_start + (total - drives_start); // was just drives til end
  // Now: loop through all items, find transitions
  int places_end   = 0; while places_end < total && kind ∉ {Favorite,Root,Drive,Network,Cloud} → ++places_end
  int fav_start    = places_end; while fav_start < total && kind == Favorite → ++fav_start
  int drives_start = fav_start; while drives_start < total && kind ∈ {Root,Drive} → ++drives_start
  int network_start = drives_start; while network_start < total && kind == Network → ++network_start
  // cloud_start = network_start; cloud items follow
```

### 1.3 Async Poll Pattern (reused for all async operations)

```
embed.cpp:468–547 main loop:
  1. mount_poll_wake (atomic bool, std::memory_order_acq_rel exchange)
  2. Check mount_result_drive_id → process, clear, set sidebar_needs_refresh
  3. Check unmount_result_drive_id → process, navigate home if viewing, refresh sidebar+computer
  4. NEW: Check iso_mount_result_device → process, refresh sidebar+computer
  5. NEW: Check iso_unmount_result_device → process, refresh
  6. NEW: Check gvfs_mount_result_path → process, refresh
  7. NEW: Check gvfs_unmount_result_url → process, refresh
  8. NEW: Check format_result_device → process, refresh

All follow the same pattern:
  thread: set pending_* → do work → set result_* + poll_wake = true
  embed: exchange poll_wake → if true, check all result fields under mount_mtx lock → process → clear
```

### 1.4 Dialog State Machine (reused for all 4 dialogs)

```
State: {Closed, Open, Running, Done, Error}
Transitions:
  Closed → Open: user action (menu item "Compress..."/"Connect to Server..."/"Format...") sets dialog_open=true
  Open → Running: user clicks action button → sets running=true, spawns async thread
  Running → Open: thread completes → sets result fields, poll_wake=true
  Running → Running: user can't interact (buttons disabled, spinner shown)
  Open → Closed: user clicks Cancel or Escape
  Done → Closed: result processed, or user acknowledges
  Open → Error: validation fails (no server, empty label, etc.) — set status text, stay open
```

### 1.5 New AppState Fields (add to `app_types.hpp` ~line 688)

```cpp
// ── ISO mount state ── (Feature 1)
std::string iso_pending_path;       // non-empty = mount in progress
bool iso_mount_success = false;
std::string iso_result_path;        // ISO file that was mounted
std::string iso_result_device;      // loop device e.g., /dev/loop0
std::string iso_result_mountpoint;  // /media/loop0/ or similar
std::string iso_unmount_pending_device;
bool iso_unmount_success = false;
std::string iso_unmount_result_device;

// ── Format drive state ── (Feature 2)
struct FormatDriveEntry {
    std::string device;             // /dev/sdb1
    std::string label;              // from partition label
    uint64_t size = 0;
};
bool format_dialog_open = false;
std::vector<FormatDriveEntry> format_dialog_drives;
int format_dialog_drive_idx = 0;    // selected index into drives
int format_dialog_fstype = 0;       // 0=ext4, 1=ntfs, 2=fat32, 3=exfat, 4=btrfs, 5=xfs, 6=f2fs
char format_dialog_label[256] = {}; // null-terminated label input
int format_dialog_label_len = 0;
int format_dialog_label_cursor = 0;
bool format_dialog_quick = true;    // true=quick, false=full (dd zero + mkfs)
int format_dialog_hover_fstype = -1;
int format_dialog_hover_btn = -1;   // 0=Cancel, 1=Format
bool format_dialog_running = false;
std::string format_dialog_status;   // status text during/after format
std::string format_pending_device;
std::string format_result_device;
bool format_success = false;
// Tool availability (checked at dialog open)
bool format_tool_available[7] = {};

// ── Network mount state ── (Features 3-5)
bool connect_dialog_open = false;
int connect_protocol = 0;           // 0=SMB, 1=SFTP, 2=FTP, 3=WebDAV, 4=AFP
char connect_server[256] = {};
int connect_server_cursor = 0;
char connect_port[8] = {};
int connect_port_cursor = 0;
char connect_share[256] = {};
int connect_share_cursor = 0;
char connect_user[128] = {};
int connect_user_cursor = 0;
char connect_pass[128] = {};        // masked in UI, cleaned from memory on close
int connect_pass_cursor = 0;
bool connect_remember = false;
int connect_hover_btn = -1;
int connect_hover_proto = -1;
bool connect_gio_available = true;  // set at startup by which gio mount

struct NetworkBookmark {
    std::string label;
    std::string url;                // URI with credentials stripped for display
    std::string protocol;           // smb, sftp, ftp, webdav, afp
};
std::vector<NetworkBookmark> network_bookmarks;

// GVfs async mount (parallels UDisks2 pattern exactly)
std::mutex gvfs_mtx;
std::string gvfs_pending_url;
std::string gvfs_result_url;
std::string gvfs_result_path;       // FUSE mount path e.g. /run/user/$UID/gvfs/smb-...
bool gvfs_success = false;
std::string gvfs_unmount_pending_url;
bool gvfs_unmount_success = false;
std::string gvfs_unmount_result_url;

// ── LAN discovery state ── (Feature 5)
struct LanShare {
    std::string name;
    std::string protocol;           // smb, sftp, ftp, webdav
    std::string server;
    int port = 0;
    std::string share;
    std::string url;                // gio-mountable URL
    std::string comment;
};
std::vector<LanShare> lan_shares;
bool lan_discovery_done = false;
uint64_t lan_discovery_last_ms = 0;
bool lan_discovery_running = false;

// ── Cloud state ── (Feature 6)
struct CloudRemote {
    std::string name;
    std::string type;               // drive, dropbox, onedrive, nextcloud, etc.
    std::string mount_path;         // ~/.local/share/horizon-files/cloud/<name>/
    bool is_mounted = false;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    bool mounting = false;
};
std::vector<CloudRemote> cloud_remotes;
bool rclone_available = false;      // set at startup
std::string cloud_pending_remote;
std::string cloud_result_remote;
bool cloud_mount_success = false;
```

### 1.6 SidebarLocation Additions (app_types.hpp:78–103)

```cpp
// Add to enum class Kind:
Network,  // GVfs network mount or discovered LAN share
Cloud,    // rclone cloud FUSE mount

// Add fields to SidebarLocation:
std::string url;            // network URL for Network/Cloud kinds
std::string device_type;    // "block", "loop", "network", "cloud", "lan"
bool needs_password = false; // for network locations
```

### 1.7 ContextMenuAction Additions (app_types.hpp:343–381)

```cpp
// Add between EmptyTrash and OpenFileLocation (after line 374):
MountISO,
UnmountISO,
FormatDrive,
ConnectToServer,
DisconnectNetwork,
RemoveBookmark,
MountCloud,
UnmountCloud,
```

### 1.8 ComputerItem::Group (already has Network at line 115)

```cpp
// Add to enum class Group:
Cloud,  // NEW
```

---

<a name="2"></a>
## 2. Feature 1: ISO / Disk Image Mounting

### 2.1 File: `iso_mounter.hpp`

```cpp
#pragma once
#include <string>
#include <functional>

namespace eh::file_browser::iso {

// Check if a file path is a mountable disk image
bool is_image_file(const std::string& path);

// Check if a loop device has a backing file (for sidebar display filtering)
bool is_loop_with_backing_file(const std::string& loop_dev);

// Get the backing file for a loop device (empty if none)
std::string get_loop_backing_file(const std::string& loop_dev);

// Async mount: runs losetup + udisksctl in thread, calls callback
void mount_iso_async(const std::string& iso_path,
                     std::function<void(bool ok, std::string device, std::string mountpoint)> cb);

// Async unmount: runs udisksctl unmount + loop-delete in thread
void unmount_iso_async(const std::string& loop_device,
                       std::function<void(bool ok)> cb);

} // namespace
```

### 2.2 File: `iso_mounter.cpp` — Complete Logic

**`is_image_file()`:**
```cpp
bool is_image_file(const std::string& path) {
    static const char* kExt[] = {".iso", ".img", ".bin", ".nrg", ".mdf", ".toast", ".raw", ".dmg", ".vhd", ".vdi"};
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return false;
    std::string ext;
    for (auto& c : std::string(path.substr(pos))) ext += std::tolower(c);
    for (auto* e : kExt) if (ext == e) return true;
    return false;
}
```

**`mount_iso_async()` — the actual async pattern:**
```cpp
void mount_iso_async(const std::string& iso_path,
                     std::function<void(bool, std::string, std::string)> cb) {
    std::thread([iso_path, cb = std::move(cb)] {
        // Step 1: Check if already mounted via losetup -j
        std::string check_cmd = "LC_ALL=C losetup -j \"" + iso_path + "\" 2>/dev/null";
        FILE* f = popen(check_cmd.c_str(), "r");
        char buf[1024];
        std::string check_out;
        while (fgets(buf, sizeof(buf), f)) check_out += buf;
        pclose(f);

        // Parse: /dev/loop0: [xxxx]:yyy (/path/to/iso)
        std::regex loop_re(R"(^(/dev/loop\d+):)");
        std::smatch m;
        std::string existing_dev;
        if (std::regex_search(check_out, m, loop_re))
            existing_dev = m[1];

        if (!existing_dev.empty()) {
            // Already set up; just mount if not already mounted
            // Check /proc/mounts
            // ... full implementation has edge cases for ro vs rw, etc.
            // For brevity: mount it
            std::string mount_cmd = "LC_ALL=C udisksctl mount -b " + existing_dev + " 2>&1";
            // ... parse "Mounted at /media/..." from output
            // If already mounted, parse that too
            if (cb) cb(true, existing_dev, mount_point);
            return;
        }

        // Step 2: losetup (read-only)
        std::string cmd = "LC_ALL=C udisksctl loop-setup -r -f \"" + iso_path + "\" 2>&1";
        f = popen(cmd.c_str(), "r");
        std::string setup_out;
        while (fgets(buf, sizeof(buf), f)) setup_out += buf;
        int rc = pclose(f);
        if (rc != 0) {
            if (cb) cb(false, "", "Setup failed: " + setup_out);
            return;
        }

        // Parse "/dev/loopN" from output
        std::regex dev_re(R"((/dev/loop\d+))");
        std::string loop_dev;
        if (std::regex_search(setup_out, m, dev_re))
            loop_dev = m[1];
        if (loop_dev.empty()) {
            if (cb) cb(false, "", "Could not determine loop device");
            return;
        }

        // Step 3: mount the loop device
        std::string mount_cmd = "LC_ALL=C udisksctl mount -b " + loop_dev + " 2>&1";
        f = popen(mount_cmd.c_str(), "r");
        std::string mount_out;
        while (fgets(buf, sizeof(buf), f)) mount_out += buf;
        pclose(f);

        // Parse "Mounted at /media/<label>"
        std::regex mount_re(R"(Mounted at\s+(.+))");
        std::string mount_point;
        if (std::regex_search(mount_out, m, mount_re))
            mount_point = m[1];
        // Trim whitespace
        mount_point.erase(0, mount_point.find_first_not_of(" \t\r\n"));
        mount_point.erase(mount_point.find_last_not_of(" \t\r\n") + 1);

        if (cb) cb(true, loop_dev, mount_point);
    }).detach();
}
```

**`unmount_iso_async()`:**
```cpp
void unmount_iso_async(const std::string& loop_dev,
                       std::function<void(bool)> cb) {
    std::thread([loop_dev, cb = std::move(cb)] {
        // Step 1: unmount
        std::string cmd = "LC_ALL=C udisksctl unmount -b " + loop_dev + " 2>&1";
        FILE* f = popen(cmd.c_str(), "r");
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) {}
        pclose(f);

        // Step 2: delete loop device
        cmd = "LC_ALL=C udisksctl loop-delete -b " + loop_dev + " 2>&1";
        f = popen(cmd.c_str(), "r");
        while (fgets(buf, sizeof(buf), f)) {}
        int rc = pclose(f);

        if (cb) cb(rc == 0);
    }).detach();
}
```

### 2.3 Integration: `embed.cpp` Poll Loop (add after unmount processing, ~line 545)

```cpp
// ── process pending ISO mount results ──
{
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    if (!app.iso_result_device.empty()) {
        // Mount completed
        app.sidebar_needs_refresh = true;
        app.computer_needs_refresh = true;
        // Optionally navigate: if iso mount was triggered by user, go there
        if (app.mount_navigate_drive_id.empty() && !app.iso_result_mountpoint.empty())
            navigate_to(app, app.iso_result_mountpoint);
        app.iso_result_device.clear();
        app.iso_result_path.clear();
        app.pendingRedraw = true;
    }
    if (!app.iso_unmount_result_device.empty()) {
        // For unmount: check if we were viewing the ISO mount point
        for (const auto& loc : app.sidebar_locations) {
            if (loc.kind == SidebarLocation::Kind::Drive && loc.is_mounted &&
                loc.drive_id == app.iso_unmount_result_device) {
                if (app.cur_tab().current_path.find(loc.path) == 0) {
                    navigate_to(app, home_dir());
                }
                break;
            }
        }
        app.sidebar_needs_refresh = true;
        app.computer_needs_refresh = true;
        app.iso_unmount_result_device.clear();
        app.pendingRedraw = true;
    }
}
```

### 2.4 Integration: `nav.cpp` — ISO Mount Async Trigger

Add after `unmount_drive()` (~line 1149):
```cpp
void mount_iso(AppState& app, const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(app.mount_mtx);
        app.iso_pending_path = path;
    }
    iso::mount_iso_async(path, [&app](bool ok, std::string dev, std::string mp) {
        std::lock_guard<std::mutex> lock(app.mount_mtx);
        if (app.iso_pending_path.empty()) return;
        app.iso_mount_success = ok;
        app.iso_result_device = std::move(dev);
        app.iso_result_mountpoint = std::move(mp);
        app.iso_result_path = std::move(app.iso_pending_path);
        app.iso_pending_path.clear();
        app.mount_poll_wake.store(true, std::memory_order_release);
    });
}

void unmount_iso(AppState& app, const std::string& loop_dev) {
    {
        std::lock_guard<std::mutex> lock(app.mount_mtx);
        app.iso_unmount_pending_device = loop_dev;
    }
    iso::unmount_iso_async(loop_dev, [&app](bool ok) {
        std::lock_guard<std::mutex> lock(app.mount_mtx);
        if (app.iso_unmount_pending_device.empty()) return;
        app.iso_unmount_success = ok;
        app.iso_unmount_result_device = std::move(app.iso_unmount_pending_device);
        app.iso_unmount_pending_device.clear();
        app.mount_poll_wake.store(true, std::memory_order_release);
    });
}
```

### 2.5 Integration: `nav.cpp` — `refresh_sidebar()` Changes (~line 984)

**Loop device filtering in /proc/mounts scan:**
```cpp
// Current code skips loop devices entirely; change to:
if (dev.find("loop") != std::string::npos) {
    // Only include if it has a real backing file (ISO mount)
    if (!iso::is_loop_with_backing_file(dev)) continue;
    // Skip snap packages (common pollution)
    std::string backing = iso::get_loop_backing_file(dev);
    if (backing.find("/snap/") != std::string::npos ||
        backing.find("/var/lib/snapd/") != std::string::npos ||
        backing.find("/usr/lib/") != std::string::npos)
        continue;
}
```

**`is_loop_with_backing_file()` implementation:**
```cpp
bool is_loop_with_backing_file(const std::string& loop_dev) {
    std::string cmd = "LC_ALL=C losetup -l -O BACK-FILE " + loop_dev + " 2>/dev/null | tail -1";
    FILE* f = popen(cmd.c_str(), "r");
    char buf[1024] = {};
    if (fgets(buf, sizeof(buf), f)) {
        pclose(f);
        std::string result(buf);
        result.erase(0, result.find_first_not_of(" \t\n\r"));
        result.erase(result.find_last_not_of(" \t\n\r") + 1);
        return !result.empty() && result != "0";
    }
    pclose(f);
    return false;
}
```

### 2.6 Integration: `menu.cpp` — Context Menu Builder

In `open_context_menu()` (~line 133), add before the separator before Properties:
```cpp
// ISO mount/unmount
if (entry_count == 1) {
    auto& entry = app.cur_tab().entries[app.context_menu_file_idx];
    if (iso::is_image_file(entry.path)) {
        // Check if already mounted
        bool already_mounted = false;
        std::string loop_dev;
        for (auto& loc : app.sidebar_locations) {
            if (loc.kind == SidebarLocation::Kind::Drive &&
                loc.drive_id.find("loop") != std::string::npos &&
                loc.path == entry.path) { // simplified; real check via losetup -j
                already_mounted = true;
                loop_dev = loc.drive_id;
                break;
            }
        }
        // Actually, we need to check via losetup -j synchronously
        // OR store a map of iso_path → loop_dev in AppState
        if (already_mounted)
            app.context_menu_items.push_back(
                AppState::menu_item(AppState::ContextMenuAction::UnmountISO, "Unmount ISO"));
        else
            app.context_menu_items.push_back(
                AppState::menu_item(AppState::ContextMenuAction::MountISO, "Mount"));
    }
}
```

### 2.7 Integration: `menu.cpp` — Action Dispatch

In `execute_context_menu_action()` (~line 165), add after mount/unmount drive section (~line 352):
```cpp
if (action == AppState::ContextMenuAction::MountISO) {
    if (app.context_menu_file_idx >= 0) {
        auto& entry = app.cur_tab().entries[app.context_menu_file_idx];
        mount_iso(app, entry.path);
    }
    goto done;
}
if (action == AppState::ContextMenuAction::UnmountISO) {
    // Find the loop device for this ISO (store mapping or check losetup -j)
    if (app.context_menu_file_idx >= 0) {
        auto& entry = app.cur_tab().entries[app.context_menu_file_idx];
        std::string loop_dev = iso::find_loop_for_image(entry.path); // sync helper
        if (!loop_dev.empty()) unmount_iso(app, loop_dev);
    }
    goto done;
}
```

### 2.8 Integration: `computer_view.cpp` — Loop Device Display

In `refresh_computer()` (~line 95), modify the loop device filter:
```cpp
// Original: if (dev.find("loop") != std::string::npos) continue;
// Change to:
if (dev.find("loop") != std::string::npos) {
    if (iso::is_loop_with_backing_file(dev)) {
        // Show it — ISO mount
        item.label = fs::path(backing_file).filename().string();
        item.icon_name = "media-optical";
        // ... add as Large item
    }
    continue; // skip snap/autoloop that have no backing file
}
```

### 2.9 Integration: `draw.cpp` — Sidebar Mount Indicator for Loop Devices

The existing mount indicator code (`draw.cpp:574–601`) handles `Kind::Drive` with a mounted-SVG icon on the right. ISO loop devices that pass the filter in refresh_sidebar() will be `Kind::Drive` with `is_mounted=true` and a `drive_id` like `/dev/loop0`, so they automatically get the mount indicator. The eject icon (mounted_svg) already renders — the click handler at `events.cpp:1697–1702` calls `unmount_drive()`. For ISOs, unmount_iso should be called instead.

**Fix in `events.cpp:1697–1702` — distinguish ISO unmount from block unmount:**
```cpp
if (loc.is_mounted) {
    double zf = app.zoom_pct / 100.0;
    int icon_left = app.sidebar_width - static_cast<int>(22.0 * zf);
    if (x >= icon_left) {
        if (loc.drive_id.find("loop") != std::string::npos)
            unmount_iso(app, loc.drive_id);
        else
            unmount_drive(app, sb_idx);
    } else {
        navigate_to(app, loc.path);
    }
}
```

---

<a name="3"></a>
## 3. Feature 2: Format Drive Dialog

### 3.1 File: `format_drive_dialog.hpp`

```cpp
#pragma once
#include "app_types.hpp" // AppState

namespace eh::file_browser {

// Populate format_dialog_drives from unmounted block devices
void refresh_format_drives(AppState& app);

// Check which mkfs tools are available (called when dialog opens)
void check_format_tools(AppState& app);

// Draw the format dialog overlay
void draw_format_dialog(AppState& app, cairo_t* cr);

// Handle click on format dialog
bool handle_format_dialog_click(AppState& app, int x, int y, uint32_t button);

// Handle key event on format dialog
bool handle_format_dialog_key(AppState& app, uint32_t key, uint32_t codepoint);

// Execute format (async)
void execute_format_async(AppState& app);

// Cancel format
void cancel_format(AppState& app);

} // namespace
```

### 3.2 File: `format_drive_dialog.cpp`

**`refresh_format_drives()` — called when dialog opens:**
```cpp
void refresh_format_drives(AppState& app) {
    app.format_dialog_drives.clear();
    // Scan /proc/mounts for unmounted partitions
    // Iterate /dev/disk/by-path/, /dev/sd*, /dev/nvme*, /dev/mmcblk*
    // For each device that is NOT mounted and NOT a loop device, add entry
    std::error_code ec;
    for (auto& entry : fs::directory_exists("/sys/block", ec) ?
         fs::directory_iterator("/sys/block", ec) : fs::directory_iterator()) {
        std::string name = entry.path().filename().string();
        if (name.find("loop") != std::string::npos) continue;
        if (name.find("ram") != std::string::npos) continue;
        // Check for partitions
        std::string dev_path = "/dev/" + name;
        for (auto& part : fs::directory_iterator(entry.path(), ec)) {
            std::string pname = part.path().filename().string();
            // Check if this partition is mounted
            bool mounted = false;
            FILE* f = setmntent("/proc/mounts", "r");
            struct mntent* mnt;
            while ((mnt = getmntent(f)) != nullptr) {
                if (std::string(mnt->mnt_fsname) == "/dev/" + pname ||
                    std::string(mnt->mnt_fsname) == dev_path) {
                    // Check it's not the root filesystem
                    if (std::string(mnt->mnt_dir) != "/") mounted = true;
                }
            }
            endmntent(f);
            if (mounted) continue;
            // Get size from /sys/block/.../size
            uint64_t sz = 0;
            // ... read size file
            app.format_dialog_drives.push_back({
                "/dev/" + pname,
                get_partition_label("/dev/" + pname),
                sz * 512
            });
        }
    }
}
```

**`draw_format_dialog()` — in-UI overlay following `draw_compress_dialog()` pattern at draw.cpp:2990:**

```cpp
void draw_format_dialog(AppState& app, cairo_t* cr) {
    int w = app.width, h = app.height;
    int dlg_w = 420, dlg_h = 340;
    int dlg_x = (w - dlg_w) / 2, dlg_y = (h - dlg_h) / 2;

    // Backdrop: semi-transparent
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35); // exact same as compress dialog: 0.35
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    // Card surface — fully opaque, rounded-2xl (10px)
    cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
    draw_rounded_rect(cr, dlg_x, dlg_y, dlg_w, dlg_h, 10);
    cairo_fill(cr);

    // Border — 1px outline at 25% opacity (same as compress dialog)
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, dlg_x + 0.5, dlg_y + 0.5, dlg_w - 1, dlg_h - 1, 9.5);
    cairo_stroke(cr);

    // Title — "Format Drive" bold 15px, exactly like compress dialog title at +20, +28
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 15);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, dlg_x + 20, dlg_y + 28);
    cairo_show_text(cr, "Format Drive");

    int content_x = dlg_x + 20;
    int content_y = dlg_y + 50;

    // ── Drive picker ──
    // Label
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_move_to(cr, content_x, content_y);
    cairo_show_text(cr, "Drive");

    // Dropdown-style row (simulated clickable button cycling through drives)
    // ... renders drive name + size
    // Click handled in handle_format_dialog_click

    int row_y = content_y + 20;
    // ── Filesystem type pills ──
    // Same pill pattern as compress dialog format buttons (draw.cpp:3038–3049)
    static const char* fs_labels[] = {"ext4", "NTFS", "FAT32", "exFAT", "Btrfs", "XFS", "F2FS"};
    // ... renders 3-4 per row, greyed if tool unavailable

    // ── Volume label input ──
    // Text input with cursor rendering (same pattern as connect dialog)

    // ── Quick/Full toggle ──
    // Pill buttons "Quick" | "Full" with hover/selected state

    // ── Warning text ──
    // "All data on /dev/sdb1 will be destroyed" — red tint if drive selected
    cairo_set_source_rgba(cr, 0.8, 0.2, 0.2, 0.8); // red warning
    cairo_move_to(cr, content_x, warning_y);
    cairo_show_text(cr, warning.c_str());

    // ── Buttons ──
    // Cancel (left) + Format (right, accent-colored)
    // ... same pattern as compress dialog
}
```

**`handle_format_dialog_click()` — follows `events.cpp:622–644`:**
```cpp
bool handle_format_dialog_click(AppState& app, int x, int y, uint32_t button) {
    if (!app.format_dialog_open) return false;
    int w = app.width, h = app.height;
    int dlg_w = 420, dlg_h = 340;
    int dlg_x = (w - dlg_w) / 2, dlg_y = (h - dlg_h) / 2;
    int content_x = dlg_x + 20;
    int content_y = dlg_y + 50;

    if (button == 0x110) { // left click
        // Drive picker click — cycle through drives
        // ... compute drive picker rect, cycle app.format_dialog_drive_idx

        // FSType pills — same calculation as draw.cpp:3039-3049
        for (int i = 0; i < 4; ++i) {
            int fmx = content_x + i * (fmt_w + fmt_gap);
            if (x >= fmx && x < fmx + fmt_w && y >= fmy && y < fmy + fmt_h) {
                if (app.format_tool_available[i]) {
                    app.format_dialog_fstype = i;
                    draw(app);
                }
                return true;
            }
        }
        // second row (indices 4-6)
        int fmy2 = fmy + fmt_h + fmt_gap;
        for (int i = 4; i < 7; ++i) {
            // ... same pattern
        }

        // Quick/Full toggle
        // ... calc rects, toggle app.format_dialog_quick

        // Cancel button
        if (/* in Cancel rect */) {
            app.format_dialog_open = false;
            draw(app);
            return true;
        }

        // Format button
        if (/* in Format rect */ && !app.format_dialog_running &&
            app.format_dialog_drive_idx >= 0) {
            execute_format_async(app);
            draw(app);
            return true;
        }

        // Escape outside dialog → close
        if (x < dlg_x || x > dlg_x + dlg_w || y < dlg_y || y > dlg_y + dlg_h) {
            app.format_dialog_open = false;
            draw(app);
            return true;
        }
    }
    return true; // consumed
}
```

**`execute_format_async()`:**
```cpp
void execute_format_async(AppState& app) {
    auto& entry = app.format_dialog_drives[app.format_dialog_drive_idx];
    app.format_dialog_running = true;
    app.format_dialog_status = "Formatting...";

    static const char* mkfs_cmds[7] = {
        "mkfs.ext4", "mkfs.ntfs", "mkfs.fat", "mkfs.exfat",
        "mkfs.btrfs", "mkfs.xfs", "mkfs.f2fs"
    };
    static const char* mkfs_fmt[7] = {
        " -F -L \"%s\" \"%s\" 2>&1",                    // ext4
        " -f -L \"%s\" \"%s\" 2>&1",                    // ntfs
        " -F32 -n \"%s\" \"%s\" 2>&1",                  // fat32
        " -n \"%s\" \"%s\" 2>&1",                       // exfat
        " -f -L \"%s\" \"%s\" 2>&1",                    // btrfs
        " -f -L \"%s\" \"%s\" 2>&1",                    // xfs
        " -f -l \"%s\" \"%s\" 2>&1"                     // f2fs
    };

    std::string device = entry.device;
    std::string label(app.format_dialog_label);
    int fstype = app.format_dialog_fstype;
    bool quick = app.format_dialog_quick;

    std::thread([&app, device, label, fstype, quick] {
        // Quick mode: just mkfs
        // Full mode: dd zeros first, then mkfs
        if (!quick) {
            std::string dd_cmd = "dd if=/dev/zero of=\"" + device +
                                 "\" bs=1M count=100 2>&1";
            FILE* f = popen(dd_cmd.c_str(), "r");
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) {}
            pclose(f);
        }

        char cmd_buf[1024];
        std::snprintf(cmd_buf, sizeof(cmd_buf),
                      (std::string(mkfs_cmds[fstype]) + mkfs_fmt[fstype]).c_str(),
                      label.c_str(), device.c_str());
        FILE* f = popen(cmd_buf, "r");
        std::string output;
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) output += buf;
        int rc = pclose(f);

        {
            std::lock_guard<std::mutex> lock(app.mount_mtx);
            app.format_success = (rc == 0);
            app.format_result_device = device;
            app.mount_poll_wake.store(true, std::memory_order_release);
        }
        if (rc != 0) {
            app.format_dialog_status = "Failed: " + output.substr(0, 120);
        } else {
            app.format_dialog_status = "Format complete";
        }
        app.format_dialog_running = false;
        app.pendingRedraw = true;
    }).detach();
}
```

### 3.3 Integration: Context Menu Trigger

In `menu.cpp` `open_context_menu()` sidebar builder (`events.cpp:2042–2074` equivalent in menu.cpp), add after Mount/Unmount:
```cpp
} else if (loc.kind == SidebarLocation::Kind::Drive) {
    // ... existing Mount/Unmount
    if (!loc.is_mounted && !loc.drive_id.empty()) {
        app.context_menu_items.push_back(
            AppState::menu_item(AppState::ContextMenuAction::FormatDrive, "Format..."));
    }
}
```

In `execute_context_menu_action()`, add handler:
```cpp
if (action == AppState::ContextMenuAction::FormatDrive) {
    if (app.context_menu_sidebar_idx >= 0) {
        auto& loc = app.sidebar_locations[app.context_menu_sidebar_idx];
        if (loc.kind == SidebarLocation::Kind::Drive && !loc.is_mounted) {
            // Only allow formatting unmounted drives
            app.format_dialog_open = true;
            refresh_format_drives(app);
            check_format_tools(app);
            // Select the right drive entry matching loc.drive_id
            for (size_t i = 0; i < app.format_dialog_drives.size(); ++i) {
                if (app.format_dialog_drives[i].device == loc.drive_id) {
                    app.format_dialog_drive_idx = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    goto done;
}
```

### 3.4 Integration: `events.cpp` — Click Routing for Format Dialog

Add at top of the mouse click handler (before sidebar click handling), just like the compress dialog routing at `events.cpp:622`:
```cpp
// ── Format dialog click ──
if (app.format_dialog_open) {
    handle_format_dialog_click(app, x, y, button);
    return;
}
```

### 3.5 Integration: `events.cpp` — Hover for Format Dialog

In the mouse motion handler, add hover state update for format dialog pills and buttons (same pattern as `compress_hover_format` in draw.cpp).

### 3.6 Integration: `draw.cpp` — Format Dialog Draw

Add at end of `draw_sidebar()` or in `draw_file_browser()` main function:
```cpp
if (app.format_dialog_open)
    draw_format_dialog(app, cr);
```

### 3.7 Integration: `embed.cpp` — Format Result Polling

Add after ISO processing:
```cpp
// ── process format results ──
{
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    if (!app.format_result_device.empty()) {
        app.sidebar_needs_refresh = true;
        app.computer_needs_refresh = true;
        app.format_result_device.clear();
        app.pendingRedraw = true;
    }
}
```

---

<a name="4"></a>
## 4. Feature 3: GVfs Network Mount Backend

### 4.1 File: `network_mounter.hpp`

```cpp
#pragma once
#include <string>
#include <functional>
#include <vector>
#include <utility>

namespace eh::file_browser::network {

// URI construction
std::string build_uri(int protocol, const std::string& server,
                       const std::string& port, const std::string& share,
                       const std::string& user, const std::string& pass);

// Check if gio mount CLI is available
bool gio_available();

// Detect currently mounted GVfs paths from /run/user/$UID/gvfs/
// Returns list of {mount_path, display_label}
std::vector<std::pair<std::string, std::string>> detect_gvfs_mounts();

// Get the protocol label for a GVfs path (for display)
std::string gvfs_protocol_label(const std::string& gvfs_path);

// Async mount via gio mount CLI
void gio_mount_async(const std::string& uri,
                     std::function<void(bool ok, std::string mount_path)> cb);

// Async unmount via gio mount -u
void gio_unmount_async(const std::string& uri,
                       std::function<void(bool ok)> cb);

} // namespace
```

### 4.2 File: `network_mounter.cpp`

**`gio_available()`:**
```cpp
bool gio_available() {
    static int available = -1;
    if (available == -1) {
        int rc = system("which gio >/dev/null 2>&1");
        available = (rc == 0) ? 1 : 0;
    }
    return available == 1;
}
```

**`build_uri()`:**
```cpp
std::string build_uri(int protocol, const std::string& server,
                       const std::string& port, const std::string& share,
                       const std::string& user, const std::string& pass) {
    // Protocol constants
    static const char* schemes[] = {"smb", "sftp", "ftp", "davs", "afp"};
    static const char* default_ports[] = {"445", "22", "21", "443", "548"};

    std::string uri = std::string(schemes[protocol]) + "://";
    if (!user.empty()) {
        uri += user;
        if (!pass.empty()) uri += ":" + pass;
        uri += "@";
    }
    uri += server;
    std::string p = port.empty() ? default_ports[protocol] : port;
    if (p != std::string(default_ports[protocol])) uri += ":" + p;
    if (protocol == 0) { // SMB
        uri += "/" + share;
    } else {
        uri += share.empty() ? "/" : "/" + share;
    }
    return uri;
}
```

**`detect_gvfs_mounts()`:**
```cpp
std::vector<std::pair<std::string, std::string>> detect_gvfs_mounts() {
    std::vector<std::pair<std::string, std::string>> result;
    std::string gvfs_dir = "/run/user/" + std::to_string(getuid()) + "/gvfs";
    std::error_code ec;
    if (!fs::is_directory(gvfs_dir, ec)) return result;
    for (auto& entry : fs::directory_iterator(gvfs_dir, ec)) {
        std::string path = entry.path().string();
        // Parse label from GVfs path format:
        // smb-share:server=host.local,share=myshare
        // sftp:host=server.local,user=user
        std::string label = path;
        auto pos = label.rfind('/');
        if (pos != std::string::npos) label = label.substr(pos + 1);
        // Generate human label: "server (SMB)"
        std::string protocol_label = label.substr(0, 3);
        // ... parse out server name
        result.push_back({path, label});
    }
    return result;
}
```

**`gio_mount_async()`:**
```cpp
void gio_mount_async(const std::string& uri,
                     std::function<void(bool, std::string)> cb) {
    std::thread([uri, cb = std::move(cb)] {
        // Build command
        // For URLs with passwords, write to temp script to avoid exposing in ps
        // Or use GIO_EXTRA_PASS env var approach
        std::string cmd = "LC_ALL=C gio mount '" + uri + "' 2>&1";
        FILE* f = popen(cmd.c_str(), "r");
        char buf[4096];
        std::string output;
        while (fgets(buf, sizeof(buf), f)) output += buf;
        int rc = pclose(f);

        if (rc != 0) {
            // Check for specific errors
            if (output.find("password") != std::string::npos ||
                output.find("Password") != std::string::npos) {
                // Need authentication — can't interact from CLI
                // Return special error
                if (cb) cb(false, "AUTH_REQUIRED");
                return;
            }
            if (cb) cb(false, output);
            return;
        }

        // Parse "Mounted at /run/user/..."
        std::regex mount_re(R"(Mounted at\s+(/run/user/\d+/gvfs/[^\s]+))");
        std::smatch m;
        std::string mount_path;
        if (std::regex_search(output, m, mount_re)) {
            mount_path = m[1];
        } else {
            // Mount may have succeeded but output format differs
            // Fallback: scan GVfs dir for newest entry
            mount_path = find_newest_gvfs_mount();
        }

        if (cb) cb(true, mount_path);
    }).detach();
}
```

**`gio_unmount_async()`:**
```cpp
void gio_unmount_async(const std::string& url_or_path,
                       std::function<void(bool)> cb) {
    std::thread([url_or_path, cb = std::move(cb)] {
        std::string cmd = "LC_ALL=C gio mount -u '" + url_or_path + "' 2>&1";
        FILE* f = popen(cmd.c_str(), "r");
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {}
        int rc = pclose(f);
        if (cb) cb(rc == 0);
    }).detach();
}
```

### 4.3 Integration: `nav.cpp` — Sidebar GVfs Mount Discovery

Add to `refresh_sidebar()` after drives section:
```cpp
// ── Network section (GVfs mounts) ──
auto gvfs_mounts = network::detect_gvfs_mounts();
for (auto& [path, label] : gvfs_mounts) {
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Network;
    loc.label = label;
    loc.path = path;
    loc.icon_name = "folder-remote";
    loc.is_mounted = true;
    loc.url = path; // store for unmount
    loc.device_type = "network";
    app.sidebar_locations.push_back(std::move(loc));
}
// Add network bookmarks from config
for (auto& bm : app.network_bookmarks) {
    // Check if already mounted before adding
    bool already = false;
    for (auto& sl : app.sidebar_locations)
        if (sl.url == bm.url) { already = true; break; }
    if (already) continue;
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Network;
    loc.label = bm.label;
    loc.path = bm.url; // placeholder until mounted
    loc.icon_name = "folder-remote";
    loc.is_mounted = false; // needs mounting
    loc.url = bm.url;
    loc.device_type = "network";
    app.sidebar_locations.push_back(std::move(loc));
}
```

### 4.4 Integration: `embed.cpp` — GVfs Result Polling

Add after format processing:
```cpp
// ── process pending GVfs mount results ──
{
    std::lock_guard<std::mutex> lock(app.gvfs_mtx);
    if (!app.gvfs_result_url.empty()) {
        if (app.gvfs_success && !app.gvfs_result_path.empty()) {
            app.sidebar_needs_refresh = true;
            app.computer_needs_refresh = true;
            app.mount_navigate_drive_id = "gvfs:" + app.gvfs_result_path;
            navigate_to(app, app.gvfs_result_path);
        }
        app.gvfs_result_url.clear();
        app.gvfs_result_path.clear();
        app.pendingRedraw = true;
    }
    if (!app.gvfs_unmount_result_url.empty()) {
        // Navigate away if viewing the unmounted path
        for (auto& sl : app.sidebar_locations) {
            if (sl.kind == SidebarLocation::Kind::Network &&
                sl.url == app.gvfs_unmount_result_url && sl.is_mounted) {
                if (app.cur_tab().current_path.find(sl.path) == 0)
                    navigate_to(app, home_dir());
                break;
            }
        }
        app.sidebar_needs_refresh = true;
        app.computer_needs_refresh = true;
        app.gvfs_unmount_result_url.clear();
        app.pendingRedraw = true;
    }
}
```

---

<a name="5"></a>
## 5. Feature 4: Connect to Server Dialog

### 5.1 File: `connect_dialog.hpp`

```cpp
#pragma once
#include "app_types.hpp"
#include <cairo/cairo.h>

namespace eh::file_browser {

void draw_connect_dialog(AppState& app, cairo_t* cr);
bool handle_connect_dialog_click(AppState& app, int x, int y, uint32_t button);
bool handle_connect_dialog_key(AppState& app, uint32_t key, uint32_t codepoint);
void execute_connect(AppState& app);

} // namespace
```

### 5.2 File: `connect_dialog.cpp` — Draw

Same pattern as format dialog:
```cpp
void draw_connect_dialog(AppState& app, cairo_t* cr) {
    int w = app.width, h = app.height;
    int dlg_w = 460, dlg_h = 400; // slightly taller for more inputs
    int dlg_x = (w - dlg_w) / 2, dlg_y = (h - dlg_h) / 2;

    // Same backdrop + card + border as compress dialog (draw.cpp:2998–3012)
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 1.0);
    draw_rounded_rect(cr, dlg_x, dlg_y, dlg_w, dlg_h, 10);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, dlg_x + 0.5, dlg_y + 0.5, dlg_w - 1, dlg_h - 1, 9.5);
    cairo_stroke(cr);

    // Title — "Connect to Server"
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 15);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, dlg_x + 20, dlg_y + 28);
    cairo_show_text(cr, "Connect to Server");

    int content_x = dlg_x + 20;
    int content_y = dlg_y + 50;

    // ── Protocol pills ──
    // Same pill pattern as compress dialog (draw.cpp:3038–3049)
    // 5 pills: SMB · SFTP · FTP · WebDAV · AFP
    // Width = (dlg_w - 40 - 4*gap) / 5

    // ── Server input ──
    // Label + text input row

    // ── Port input ──
    // Auto-filled, narrower

    // ── Share/Path input ──

    // ── Username input ──

    // ── Password input ── (masked)

    // ── "Remember this connection" checkbox ──

    // ── Cancel + Connect buttons ──
}
```

### 5.3 Integration: `events.cpp` — Connect Dialog Click Handling

Add at top of click handler (before format dialog check):
```cpp
if (app.connect_dialog_open) {
    handle_connect_dialog_click(app, x, y, button);
    return;
}
```

### 5.4 Integration: `menu.cpp` — Trigger

Add "Connect to Server..." to background context menu (`events.cpp:1319` area), and to the dots menu:
```cpp
// Background context menu → add between Copy Location and Separator
app.context_menu_items.push_back(
    AppState::menu_item(AppState::ContextMenuAction::ConnectToServer,
                        "Connect to Server\u2026"));
```

In `execute_context_menu_action()`:
```cpp
if (action == AppState::ContextMenuAction::ConnectToServer) {
    app.connect_dialog_open = true;
    // Reset fields
    app.connect_protocol = 0;
    app.connect_server[0] = '\0';
    std::strcpy(app.connect_port, "445");
    app.connect_share[0] = '\0';
    app.connect_user[0] = '\0';
    app.connect_pass[0] = '\0';
    app.connect_remember = false;
    goto done;
}
```

### 5.5 Sidebar Network Click Handling

In `events.cpp:1679–1731`, add `Kind::Network` handling:
```cpp
} else if (loc.kind == SidebarLocation::Kind::Network) {
    if (loc.is_mounted) {
        navigate_to(app, loc.path);
    } else if (network::gio_available()) {
        // Async mount via GVfs
        {
            std::lock_guard<std::mutex> lock(app.gvfs_mtx);
            app.gvfs_pending_url = loc.url;
        }
        network::gio_mount_async(loc.url, [&app](bool ok, std::string mp) {
            std::lock_guard<std::mutex> lock(app.gvfs_mtx);
            if (app.gvfs_pending_url.empty()) return;
            app.gvfs_success = ok;
            app.gvfs_result_url = std::move(app.gvfs_pending_url);
            app.gvfs_result_path = std::move(mp);
            app.gvfs_pending_url.clear();
            app.mount_poll_wake.store(true, std::memory_order_release);
        });
    }
}
```

### 5.6 Sidebar Network Right-Click Context Menu

In sidebar right-click handler (`events.cpp:2042–2074`), add:
```cpp
} else if (loc.kind == SidebarLocation::Kind::Network) {
    if (loc.is_mounted && !loc.url.empty()) {
        app.context_menu_items.push_back(
            AppState::menu_item(AppState::ContextMenuAction::DisconnectNetwork, "Disconnect"));
    }
    // Bookmarks can be edited
}
```

### 5.7 Persistence: `shell_config.hpp`

Add to `FileBrowserSettings`:
```cpp
std::vector<std::string> network_bookmark_labels;
std::vector<std::string> network_bookmark_urls;
std::vector<std::string> network_bookmark_protocols;
```

Serialization in `write_file_browser_toml()` / `read_file_browser_toml()`:
```cpp
// Write
std::vector<toml_value> bm_labels, bm_urls, bm_protos;
for (auto& bm : app.network_bookmarks) {
    bm_labels.push_back(toml_value(bm.label));
    bm_urls.push_back(toml_value(bm.url));
    bm_protos.push_back(toml_value(bm.protocol));
}
// ... write to TOML table
```

---

<a name="6"></a>
## 6. Feature 5: LAN Network Browsing (Discovery)

### 6.1 File: `lan_discovery.hpp`

```cpp
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace eh::file_browser::lan {

struct LanShare {
    std::string name;
    std::string protocol;  // smb, sftp, ftp, webdav
    std::string server;
    int port = 0;
    std::string share;
    std::string url;        // gio-mountable URL
    std::string comment;
    std::string workgroup;
};

// Detect which discovery backends are available
bool avahi_available();
bool ws_discovery_available();
bool nmblookup_available();

// Run all available discovery methods, return merged results
std::vector<LanShare> discover_all();

// Individual backends
std::vector<LanShare> discover_via_avahi();
std::vector<LanShare> discover_via_wsd();
std::vector<LanShare> discover_via_samba();

} // namespace
```

### 6.2 File: `lan_discovery.cpp`

**`discover_via_avahi()`:**
```cpp
std::vector<LanShare> discover_via_avahi() {
    std::vector<LanShare> result;
    FILE* f = popen("avahi-browse -at 2>/dev/null", "r");
    if (!f) return result;
    char buf[4096];
    std::string cur_service, cur_type, cur_host;
    int cur_port = 0;
    std::string cur_txt;
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        // Parse avahi-browse output:
        // +;eth0;IPv6;Server;WebDAV Share;local;davs;local
        // =;eth0;IPv4;Server _smb._tcp;local;HOST.local;445;
        if (line[0] == '+') {
            // Service appeared
        } else if (line[0] == '=') {
            // Resolved service — extract host, port, TXT
            auto parts = split(line, ';');
            if (parts.size() >= 8) {
                std::string type = parts[3];
                std::string host = parts[5];
                int port = std::stoi(parts[7]);
                // Map service type to protocol
                std::string protocol;
                if (type.find("_smb") != std::string::npos) protocol = "smb";
                else if (type.find("_sftp") != std::string::npos) protocol = "sftp";
                else if (type.find("_ftp") != std::string::npos) protocol = "ftp";
                else if (type.find("_webdav") != std::string::npos) protocol = "webdav";
                else if (type.find("_afp") != std::string::npos) protocol = "afp";
                else continue;

                std::string name = parts[2] + " (" + protocol + ")";
                LanShare share;
                share.name = name;
                share.protocol = protocol;
                share.server = host;
                share.port = port;
                share.share = type.find("_smb") != std::string::npos ? host : "";
                share.url = build_url(protocol, host, std::to_string(port), share.share, "", "");
                result.push_back(std::move(share));
            }
        }
    }
    pclose(f);
    return result;
}
```

**`discover_via_samba()`:**
```cpp
std::vector<LanShare> discover_via_samba() {
    std::vector<LanShare> result;
    // Step 1: find workgroups via nmblookup
    FILE* f = popen("nmblookup -S WORKGROUP 2>/dev/null | grep -E '<00>|<20>'", "r");
    // ... parse IPs
    // Step 2: for each server, smbclient -L <ip>
    // ... parse share list
    return result;
}
```

### 6.3 Integration: Refresh Cycle

In `computer_view.cpp`, in `refresh_computer()`, replace the Network placeholder:
```cpp
// ── Splitter: Network (was placeholder) ──
{
    ComputerItem split;
    split.shape = ComputerItem::ShapeType::Splitter;
    split.group = ComputerItem::Group::Network;
    split.label = "Network";
    app.computer_items.push_back(split);
}

// Discovered LAN shares
uint64_t now = /* chrono */;
if (!app.lan_discovery_done && !app.lan_discovery_running) {
    app.lan_discovery_running = true;
    std::thread([&app] {
        auto shares = lan::discover_all();
        {
            std::lock_guard<std::mutex> lock(app.mount_mtx); // reuse mount_mtx for simplicity
            app.lan_shares = std::move(shares);
            app.lan_discovery_done = true;
            app.lan_discovery_last_ms = current_time_ms();
            app.computer_needs_refresh = true;
        }
        app.pendingRedraw = true;
    }).detach();
}

// Use cached shares
for (auto& share : app.lan_shares) {
    ComputerItem item;
    item.shape = ComputerItem::ShapeType::Small; // or Large
    item.group = ComputerItem::Group::Network;
    item.label = share.name;
    item.icon_name = "folder-remote";
    item.path = share.url; // gio-mountable
    item.is_mounted = false; // not mounted yet
    app.computer_items.push_back(item);
}
```

In `nav.cpp` `refresh_sidebar()`, add LAN shares:
```cpp
// ── LAN discovered shares (under Network section) ──
for (auto& share : app.lan_shares) {
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Network;
    loc.label = share.name;
    loc.path = share.url;
    loc.icon_name = "network-server";
    loc.is_mounted = false;
    loc.url = share.url;
    loc.device_type = "lan";
    app.sidebar_locations.push_back(std::move(loc));
}
```

### 6.4 Re-scan Timer

In `embed.cpp` main loop, periodically trigger LAN re-discovery:
```cpp
uint64_t now = /* chrono */;
if (app.lan_discovery_done && !app.lan_discovery_running &&
    now - app.lan_discovery_last_ms > 60000) {
    app.lan_discovery_done = false;
    app.computer_needs_refresh = true; // triggers new discovery thread
}
```

---

<a name="7"></a>
## 7. Feature 6: Cloud Storage (rclone)

### 7.1 File: `cloud_mounter.hpp`

```cpp
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace eh::file_browser::cloud {

struct CloudRemote {
    std::string name;
    std::string type;       // drive, dropbox, onedrive, nextcloud, s3, etc.
    std::string mount_path; // local FUSE mount directory
    bool is_mounted = false;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    std::string status;     // "mounted", "unmounted", "error:..."
};

// Check if rclone is installed
bool rclone_available();

// Parse ~/.config/rclone/rclone.conf for remotes
std::vector<CloudRemote> list_remotes();

// Check if a remote is already mounted (by checking mount_path)
bool is_mounted(const std::string& mount_path);

// Get usage stats from mountpoint (statvfs)
void get_usage(const std::string& mount_path, uint64_t& total, uint64_t& used);

// Mount remote async
void mount_async(const std::string& remote_name, const std::string& type,
                 std::function<void(bool ok)> cb);

// Unmount remote async
void unmount_async(const std::string& mount_path,
                   std::function<void(bool ok)> cb);

} // namespace
```

### 7.2 File: `cloud_mounter.cpp`

```cpp
bool rclone_available() {
    static int avail = -1;
    if (avail == -1) {
        int rc = system("which rclone >/dev/null 2>&1");
        avail = (rc == 0) ? 1 : 0;
    }
    return avail == 1;
}

std::vector<CloudRemote> list_remotes() {
    std::vector<CloudRemote> result;
    FILE* f = popen("rclone config file 2>/dev/null", "r");
    if (!f) return result;
    char buf[4096];
    std::string config_path;
    while (fgets(buf, sizeof(buf), f)) {
        if (std::string(buf).find("Configuration file") != std::string::npos) {
            // Parse: Configuration file is at /home/user/.config/rclone/rclone.conf
            // ...
        }
    }
    pclose(f);
    // Parse the config file
    // Format: [remote_name]
    //         type = drive
    //         ...
    if (config_path.empty()) return result;
    std::ifstream ifs(config_path);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line[0] == '[') {
            CloudRemote remote;
            remote.name = line.substr(1, line.size() - 2);
            // Read next line for type
            if (std::getline(ifs, line) && line.find("type = ") == 0) {
                remote.type = line.substr(7);
            }
            std::string mount_dir = getenv("HOME") + std::string("/.local/share/horizon-files/cloud/") + remote.name;
            remote.mount_path = mount_dir;
            remote.is_mounted = is_mounted(remote.mount_path);
            if (remote.is_mounted)
                get_usage(remote.mount_path, remote.total_bytes, remote.used_bytes);
            result.push_back(std::move(remote));
        }
    }
    return result;
}

void mount_async(const std::string& remote_name, const std::string& type,
                 std::function<void(bool ok)> cb) {
    std::thread([remote_name, type, cb = std::move(cb)] {
        std::string mount_dir = std::string(getenv("HOME")) +
            "/.local/share/horizon-files/cloud/" + remote_name;
        // Create mountpoint
        fs::create_directories(mount_dir);
        // Mount
        std::string cmd = "rclone mount " + remote_name + ": \"" + mount_dir +
                          "\" --daemon --vfs-cache-mode full 2>&1";
        FILE* f = popen(cmd.c_str(), "r");
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {}
        int rc = pclose(f);
        if (cb) cb(rc == 0);
    }).detach();
}

void unmount_async(const std::string& mount_path,
                   std::function<void(bool ok)> cb) {
    std::thread([mount_path, cb = std::move(cb)] {
        std::string cmd = "fusermount -u \"" + mount_path + "\" 2>&1";
        int rc = system(cmd.c_str());
        if (rc != 0) {
            cmd = "rclone unmount \"" + mount_path + "\" 2>&1";
            rc = system(cmd.c_str());
        }
        if (cb) cb(rc == 0);
    }).detach();
}
```

### 7.3 Integration: Sidebar "Cloud" Section

In `nav.cpp` `refresh_sidebar()`, after Network section:
```cpp
// ── Cloud section ──
if (!app.cloud_remotes.empty()) {
    for (auto& remote : app.cloud_remotes) {
        SidebarLocation loc;
        loc.kind = SidebarLocation::Kind::Cloud;
        loc.label = remote.name + " (" + remote.type + ")";
        loc.path = remote.mount_path;
        loc.icon_name = remote.type == "drive" ? "google-drive" :
                        remote.type == "dropbox" ? "dropbox" :
                        remote.type == "onedrive" ? "onedrive" : "drive-remote";
        loc.is_mounted = remote.is_mounted;
        loc.total_bytes = remote.total_bytes;
        loc.free_bytes = remote.total_bytes - remote.used_bytes;
        loc.device_type = "cloud";
        app.sidebar_locations.push_back(std::move(loc));
    }
}
```

### 7.4 Computer View "Cloud" Section

In `computer_view.cpp`, add after Network section:
```cpp
// ── Splitter: Cloud ──
{
    ComputerItem split;
    split.shape = ComputerItem::ShapeType::Splitter;
    split.group = ComputerItem::Group::Cloud;
    split.label = "Cloud Storage";
    app.computer_items.push_back(split);
}
for (auto& remote : app.cloud_remotes) {
    ComputerItem item;
    item.shape = ComputerItem::ShapeType::Small;
    item.group = ComputerItem::Group::Cloud;
    item.label = remote.name;
    item.icon_name = remote.type == "drive" ? "google-drive" : "drive-remote";
    item.path = remote.mount_path;
    item.is_mounted = remote.is_mounted;
    item.total_bytes = remote.total_bytes;
    item.used_bytes = remote.used_bytes;
    item.show_progress = remote.is_mounted && remote.total_bytes > 0;
    app.computer_items.push_back(item);
}
```

### 7.5 Cloud Right-Click → Mount/Unmount

In sidebar right-click handler (`events.cpp:2042–2074`), add:
```cpp
} else if (loc.kind == SidebarLocation::Kind::Cloud) {
    if (loc.is_mounted) {
        app.context_menu_items.push_back(
            AppState::menu_item(AppState::ContextMenuAction::UnmountCloud, "Unmount Cloud"));
    } else {
        app.context_menu_items.push_back(
            AppState::menu_item(AppState::ContextMenuAction::MountCloud, "Mount Cloud"));
    }
}
```

---

<a name="8"></a>
## 8. Build System

### 8.1 `meson.build` Changes (line 63–137)

Add after existing features (after line 80):
```meson
# Phase 10: Removable Media & Network
'src/app/file_browser/features/iso_mounter.cpp',
'src/app/file_browser/features/format_drive_dialog.cpp',
'src/app/file_browser/features/connect_dialog.cpp',
'src/app/file_browser/features/network_mounter.cpp',
'src/app/file_browser/features/lan_discovery.cpp',
'src/app/file_browser/features/cloud_mounter.cpp',
```

---

<a name="9"></a>
## 9. Implementation Order & Dependencies

| Step | Feature | Prereqs | Files to Create | Files to Modify | Est. LOC |
|------|---------|---------|-----------------|-----------------|----------|
| 1 | ISO mounting | None | `iso_mounter.hpp/cpp` | `app_types.hpp`, `nav.cpp`, `menu.cpp`, `embed.cpp`, `draw.cpp`, `events.cpp`, `computer_view.cpp` | ~350 |
| 2 | Format dialog | Step 1 (drive enum) | `format_drive_dialog.hpp/cpp` | `app_types.hpp`, `menu.cpp`, `embed.cpp`, `events.cpp`, `draw.cpp` | ~700 |
| 3 | GVfs network backend | None | `network_mounter.hpp/cpp` | `app_types.hpp`, `nav.cpp`, `embed.cpp`, `computer_view.cpp` | ~400 |
| 4 | Connect dialog | Step 3 | `connect_dialog.hpp/cpp` | `app_types.hpp`, `menu.cpp`, `events.cpp`, `draw.cpp`, `shell_config.hpp` | ~500 |
| 5 | LAN discovery | Step 3 (uses gio mount) | `lan_discovery.hpp/cpp` | `computer_view.cpp`, `nav.cpp`, `embed.cpp` | ~500 |
| 6 | Cloud storage | Step 3 (FUSE pattern) | `cloud_mounter.hpp/cpp` | `app_types.hpp`, `nav.cpp`, `menu.cpp`, `events.cpp`, `computer_view.cpp` | ~400 |
| 7 | Build + config | All steps | — | `meson.build`, `shell_config.hpp` | ~30 |

**Parallelism:** Steps 1+3 can run in parallel. Steps 2+4+6 can each start as soon as their backend (1 and 3) is done. Step 5 depends on 3.

---

<a name="10"></a>
## 10. Test Strategy

### 10.1 Runtime Verification (manual testing checklist)

| Test | Expected | How to Verify |
|------|----------|---------------|
| ISO mount | Sidebar shows loop device, navigates to mount | `losetup -j /path/to.iso`, `df -h` |
| ISO unmount | Sidebar removes loop device, navigate home | `losetup -a` shows nothing |
| ISO unmount via mount indicator click | Same as above | Click eject icon on ISO sidebar item |
| Format dialog opens | Overlay appears | Right-click unmounted drive → Format... |
| Format with ext4 | Drive formatted, appears in sidebar | `blkid /dev/sdX1` shows ext4 |
| Format with unavailable fstype | Button greyed out | Uninstall mkfs.ntfs, verify greyed |
| Connect to Server dialog | Overlay appears | Dots menu → Connect to Server... |
| SMB mount success | Sidebar shows Network item, navigates | `gio mount` shows the share |
| SMB mount failure (bad server) | Error message in dialog | Enter invalid hostname |
| SMB mount with password | Mount succeeds | Verify with credentials |
| LAN discovery (Avahi) | Shares appear in computer view | Run `avahi-browse -at` manually |
| LAN discovery (Samba fallback) | Shares appear | Disable avahi |
| Cloud mount (rclone) | Cloud section appears, mount works | `rclone config` set up, click Mount |
| Cloud unmount | Cloud section updates | Click Unmount |
| Network bookmark persistence | Bookmarks survive restart | Check TOML file |
| All dialogs → Escape | Dialog closes | Press Escape |
| All dialogs → click outside | Dialog closes | Click outside card |
| Loop device filtering | Only user ISOs shown in sidebar | Mount snap loop device, verify hidden |

### 10.2 Edge Cases to Test

- **ISO:** File path with spaces, special chars, very long path, .iso mounted read-only already, .iso that has been deleted after loop-setup, snap/package-manager loop device pollution detection
- **ISO:** Multiple ISOs mounted simultaneously, unmount while browsing the ISO, umount when ISO is the current directory
- **Format:** Root filesystem in list (should be excluded), format device that was just mounted, format during file transfer, format with no mkfs tool installed
- **Format:** Full mode vs quick mode timing, cancel during format (should be disabled), label with spaces/special chars, very long label
- **Network mount:** Server unreachable, wrong port, wrong protocol, password with special chars (shell escaping), mount while already mounted, unmount while browsing, reconnect after network drop
- **LAN discovery:** No discovery tools installed, multiple Samba servers, duplicate entries from multiple backends, stale entries after server goes offline
- **Cloud:** rclone not installed, rclone config empty, already-mounted remote, `fusermount` vs `rclone unmount`, S3 remotes (different CLI flags)
- **All sidebar sections:** scrollbar with 6 sections, sidebar scale computation, empty sections, section order after adding/removing favorites, drag-and-drop favorites with network/cloud sections present
- **Thread safety:** All async callbacks must lock the correct mutex before touching AppState; double-check all std::thread usage for detached-thread safety (AppState must outlive threads — use main-loop polling pattern)

---

<a name="11"></a>
## 11. Risks & Edge Cases

### 11.1 Thread Safety Analysis

```
Mount mutex (app.mount_mtx): Covers mount_pending_drive_id, mount_result_drive_id,
    unmount_pending_drive_id, unmount_result_drive_id, mount_success, unmount_success,
    iso_* fields, format_* fields
GVfs mutex (app.gvfs_mtx): Covers gvfs_pending_url, gvfs_result_url, gvfs_result_path,
    gvfs_success, gvfs_unmount_*
LAN mutex (app.mount_mtx or own): covers lan_shares, lan_discovery_done, lan_discovery_last_ms

Lock ordering: Always lock mount_mtx first, then gvfs_mtx (never reverse to avoid deadlock)
```

### 11.2 Tool Detection at Startup

Add to `app.cpp` initialization:
```cpp
app.rclone_available = cloud::rclone_available();
app.connect_gio_available = network::gio_available();
```

If `gio` is not available, disable all network features:
- "Connect to Server..." menu item greyed out
- LAN discovery skipped
- Cloud sidebar section hidden
- Computer view Network section shows "Install GVfs" message

If `rclone` is not available:
- Cloud sidebar section hidden entirely
- Computer view Cloud section shows setup message "Install rclone to use cloud storage"

### 11.3 Security

- **Passwords in memory:** Clear `connect_pass` buffer when dialog closes or app exits
- **Passwords in TOML:** Store bookmark URLs WITHOUT inline passwords; use `libsecret` for persistent credentials
- **`gio mount` with passwords:** Use `GIO_EXTRA_PASS` environment variable or stdin piping, not inline in command string
- **Shell injection:** All paths and URLs passed to `popen()` must be shell-escaped: wrap in single quotes and escape embedded single quotes

### 11.4 Desktop File Watching (GVfs auto-refresh)

Mount/unmount events for Network and Cloud locations can be detected via:
- `inotify` on `/run/user/$UID/gvfs/` directory
- `GLib` gvolume monitor (but we avoid GLib dependency)
- Periodic polling every 5 seconds in embed.cpp main loop

Suggestion: Simple polling approach — add `app.gvfs_needs_refresh = true` every 5 seconds, which triggers GVfs scan in the next refresh.

### 11.5 Section Boundary Computation (Critical for Sidebar)

The sidebar draw code at `draw.cpp:420–434` uses sequential section boundaries:
```cpp
int places_end = 0;
while (places_end < total &&
       app.sidebar_locations[places_end].kind != ...)
    ++places_end;
```

This means `sidebar_locations` MUST be ordered: Places → Favorites → Drives → Network → Cloud.
`refresh_sidebar()` MUST maintain this order. The places_end/fav_start/drives_start computation must be updated to also compute `network_start` and `cloud_start` for section height calculation:

```cpp
int total = static_cast<int>(app.sidebar_locations.size());

// Section boundaries
int places_end = 0;
while (places_end < total &&
       app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Favorite &&
       app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Root &&
       app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Drive &&
       app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Network &&
       app.sidebar_locations[places_end].kind != SidebarLocation::Kind::Cloud)
    ++places_end;

int fav_start = places_end;
while (fav_start < total &&
       app.sidebar_locations[fav_start].kind == SidebarLocation::Kind::Favorite)
    ++fav_start;

int drives_start = fav_start;
while (drives_start < total &&
       (app.sidebar_locations[drives_start].kind == SidebarLocation::Kind::Root ||
        app.sidebar_locations[drives_start].kind == SidebarLocation::Kind::Drive))
    ++drives_start;

int network_start = drives_start;
while (network_start < total &&
       app.sidebar_locations[network_start].kind == SidebarLocation::Kind::Network)
    ++network_start;

int cloud_start = network_start;
while (cloud_start < total &&
       app.sidebar_locations[cloud_start].kind == SidebarLocation::Kind::Cloud)
    ++cloud_start;
```

And the height computation and drawing code must include Network and Cloud sections:
```cpp
int n_count = network_start - drives_start;  // was drive_count = total - drives_start
int c_count = cloud_start - network_start;

int total_needed = header_h + p_count * item_h +
                   div_total + header_h + f_count * item_h +
                   div_total + header_h + d_count * item_h +
                   div_total + header_h + n_count * item_h +   // Network section
                   div_total + header_h + c_count * item_h;     // Cloud section
```

Draw sections (add after Drives section draw at `draw.cpp:741–747`):
```cpp
// ── NETWORK ──
if (network_start < cloud_start) {
    draw_divider();
    draw_header("NETWORK");
    for (int i = network_start; i < cloud_start; ++i)
        draw_item(i);
}

// ── CLOUD ──
if (cloud_start < total) {
    draw_divider();
    draw_header("CLOUD");
    for (int i = cloud_start; i < total; ++i)
        draw_item(i);
}
```

---

## Appendix A: Quick Reference — All Code Refs by File

```
app_types.hpp:
  Line  79:  SidebarLocation::Kind enum (add Network, Cloud)
  Line 105:  ComputerItem::Group enum (has Network, add Cloud)
  Line 343:  ContextMenuAction enum (add 7 actions)
  Line 484:  Compress dialog fields (~10 lines, pattern for all dialogs)
  Line 687:  mount_poll_wake atomic(bool)
  ~line 688: add all phase-10 fields (~100 lines)

nav.cpp:
  Line 939:  refresh_sidebar() start
  Line 984:  drive scanning in /proc/mounts
  Line 1105: mount_drive() — async pattern reference
  Line 1128: unmount_drive() — async pattern reference

menu.cpp:
  Line  40:  open_context_menu() builder
  Line 165:  execute_context_menu_action() dispatch
  Line 336:  MountDrive handler
  Line 345:  UnmountDrive handler
  Line 387:  AddToFavorites handler

embed.cpp:
  Line 468:  mount poll loop start
  Line 471:  mount_poll_wake exchange
  Line 490:  mount result processing
  Line 512:  unmount result processing

draw.cpp:
  Line 413:  draw_sidebar() start
  Line 419:  section boundary computation
  Line 462:  draw_item() lambda — sidebar item render
  Line 573:  mount indicator (eject icon)
  Line 607:  draw_header() — section header
  Line 618:  draw_divider() — section separator
  Line 627:  PLACES section draw
  Line 632:  FAVORITES section draw
  Line 741:  DRIVES section draw
  Line 2990: draw_compress_dialog() — dialog pattern

events.cpp:
  Line 622:  compress dialog click handling
  Line 1679: sidebar left-click handling
  Line 1697: mount indicator click (eject vs nav)
  Line 1805: computer view click handling
  Line 2042: sidebar right-click context menu
  Line 2849: sidebar hover for mount indicator

computer_view.cpp:
  Line  54:  refresh_computer() start
  Line  95:  Disks section
  Line 115:  ComputerItem::Group::UserDirs
  Line 272:  Network placeholder (no-op)
  Line 278:  draw_computer_view() start
  Line 561:  hit_test_computer() start

shell_config.hpp:
  Line 274:  FileBrowserSettings struct start
  Line 298:  favorites serialization

meson.build:
  Line  63:  sources = files(...)
  Line  80:  last feature source before Phase 10
```

---

## Appendix B: Dialog Key Handling (standard pattern)

All 4 dialogs (compress, format, connect, ISO-progress) must handle these keys identically:

| Key | Action |
|-----|--------|
| Escape | Close dialog (set dialog_open = false, draw) |
| Enter | Execute primary action (format/connect/mount) |
| Tab | Cycle focus (hover) between interactive elements |
| Left/Right | Within input fields: move cursor; between pills: switch selection |
| Backspace | Delete character before cursor |
| Delete | Delete character at cursor |
| Printable | Insert character at cursor |

Add a `handle_dialog_key()` dispatch in `events.cpp` key handler, routing to the appropriate dialog's handler based on which `dialog_open` flag is true.

---

## Appendix C: Icon Names

| Feature | Icon Name | Context |
|---------|-----------|---------|
| ISO file | `media-optical` | File icon / context menu |
| Loop device (mounted) | `media-optical` | Sidebar item icon |
| Network share | `folder-remote` | Sidebar / computer view |
| LAN server | `network-server` | Computer view small item |
| Cloud storage | `drive-remote` | Sidebar / computer view |
| Google Drive | `google-drive` | Cloud sidebar item |
| Dropbox | `dropbox` | Cloud sidebar item |
| OneDrive | `onedrive` | Cloud sidebar item |
| Format | `drive-harddisk` | Format dialog drive entries |
| Connect to Server | `network-workgroup` | Menu item icon |



---

**End Phase 10 Implementation Plan** — 6 features, 8 new files, 12 modified files, ~3,500–5,000 LOC
