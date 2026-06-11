# Horizon-files Changelog

## Phase 1: Core Interaction

- [x] Double-click to open files/folders -- 400ms debounce, <8px movement threshold
- [x] Arrow key navigation -- Up/Down/Left/Right scroll and select, Return opens, Backspace goes up
- [x] Keyboard shortcuts -- Ctrl+C/V/X (copy/cut/paste), Delete (trash), F2 (rename), Ctrl+H (hidden),
      Alt+Up/Left/Right (parent/back/forward), Ctrl+A (select all), Ctrl+Shift+N (new folder), F5 (reload)
- [x] Rename (F2) -- inline prompt via xdg, undo-supported
- [x] New folder (Ctrl+Shift+N) -- inline create dialog with undo
- [x] Move to trash (Delete) -- confirm dialog, xdg::trash_file per path
- [x] Permanent delete (Shift+Delete) -- confirm dialog, fs::remove_all / fs::remove
- [x] Cut/Copy/Paste -- clipboard service with cut/copy distinction, undo-supported paste
- [x] Context menu (right-click) -- Open, Rename, Move to Trash, Permanent Delete, Cut, Copy, Paste,
      New Folder, Properties (external), Sort submenu, Copy Path, Open in Terminal, Open With submenu
- [x] Status bar -- item count, selection info, free disk space via statvfs

## Phase 2: Navigation & Views

- [x] Breadcrumb nav bar -- clickable path segments in top bar, each navigates to that directory
- [x] Editable location bar (Ctrl+L) -- full inline path editor with cursor, selection, Home/End, Backspace,
      Delete, Ctrl+A, Ctrl+C support, Enter navigates
- [x] Zoom in/out (Ctrl+=/Ctrl+-, Ctrl+0 reset) -- 50%-200% range, scales icons, fonts, spacing
- [x] View toggle (Ctrl+1 list, Ctrl+2 grid) -- button in top bar, keyboard shortcuts
- [x] Sort by submenu -- dropdown with Name/Size/Date/Type + ascending/descending toggle
- [x] Column resizing in list view -- draggable dividers between Name/Size/Date/Type column headers
- [x] Show hidden files (Ctrl+H) -- toggle persisted to settings
- [x] Per-folder view mode memory -- per-directory view_mode, sort_field, sort_descending saved to TOML
- [x] Extended selection -- Shift-click range, Ctrl-click toggle, rubber-band marquee drag
- [~] Column visibility toggle -- state fields exist (col_show_name/size/date/type), no UI to change them

## Phase 3: Tabs

- [ ] Tabbed browsing -- not implemented
- [ ] Ctrl+T / Ctrl+W / Ctrl+Tab / Ctrl+Shift+Tab -- not implemented
- [ ] Tab reorder, middle-click close, tab context menu -- not implemented

## Phase 4: Sidebar & Places

- [x] Home shortcut
- [x] Desktop shortcut
- [x] XDG user directories (Documents, Downloads, etc.)
- [x] Trash shortcut
- [x] Root (/) shortcut
- [x] Drives / removable media -- UDisks2 async mount + /proc/mounts scanning + /dev/disk/by-label and
      by-partlabel enumeration
- [x] Bookmarks / Favorites -- add/remove via context menu, drag-to-add from file list, persisted to TOML
- [x] Drag-and-drop to sidebar -- folders and favorites accept drops, destination-aware
- [x] Mount/unmount in sidebar -- mount on click, unmount on click (toggle) or right-click → Unmount menu item
- [ ] Network locations -- not implemented
- [ ] Recent files -- not implemented
- [ ] Drive free space indicator in sidebar -- fields exist, not populated (status bar only)

## Phase 5: Advanced File Operations

- [x] Undo (Ctrl+Z) -- in-memory operation stack supporting PasteCopy, PasteCut, Rename, NewFolder,
      NewFile, MoveToTrash
- [~] File conflict resolution -- auto-rename with " (n)" suffix, no interactive dialog
- [ ] Duplicate file -- not implemented
- [ ] Create symlink -- not implemented
- [ ] Copy path to clipboard -- not implemented
- [ ] Redo (Ctrl+Shift+Z / Ctrl+Y) -- not implemented
- [ ] Copy/Move progress bar -- state fields exist, operations are synchronous
- [ ] New file from template -- not implemented
- [ ] Extract archive -- not implemented
- [ ] Compress / create archive -- not implemented
- [ ] Batch rename -- not implemented

## Phase 6: Properties & Metadata

- [x] Folder item count -- recursive_directory_iterator for selected folder, displayed in status bar
- [~] Properties dialog -- delegates to external xdg dialog, no custom inline dialog
- [~] MIME type display -- detected via xdg-mime and stored, Type column shows FileType enum (hidden by default)
- [~] Disk usage -- statvfs free space shown as text in status bar, no graphical visualization
- [ ] Permissions display / editing -- not implemented
- [ ] Owner/group display -- not implemented
- [ ] Image dimensions, audio/video duration -- not implemented

## Phase 7: Search & Filter

(All not implemented. search_query field and filter logic exist in code but no UI or keybinding activates them.)

- [ ] Quick search (type-to-find)
- [ ] Search bar (Ctrl+F)
- [ ] Filter by name inline
- [ ] Search by file type / date / size
- [ ] Recursive subdirectory search
- [ ] Saved searches

## Phase 8: Preview & Thumbnails

- [x] Image thumbnails -- progressive lazy loading with LRU-backed in-memory cache (64 MB limit),
      WebP disk cache via cache_webp_path_for_source, max 16 decodes per frame + 200 per loop iteration,
      0ms poll timeout when queue non-empty
- [x] Video thumbnails -- ffmpeg frame extraction for .mp4, .webm, .mkv, .mov, .avi
- [x] Thumbnail cache management -- in-memory LRU with stride-correct byte accounting, WebP disk cache
      with transparent/corrupt/oversize detection and automatic purge
- [~] Thumbnail size configuration -- controlled by zoom_pct (list: 24px * zf, grid: 48px * zf),
      no separate thumbnail size setting
- [~] Space/Enter preview -- opens in default application, no inline preview viewer
- [ ] PDF thumbnails -- not implemented
- [ ] File hover preview -- not implemented
- [ ] Information panel (F11) -- not implemented

## Phase 9: Advanced Views & Panels

(All not implemented. ViewMode enum supports only List and Grid.)

- [ ] Split/pane view (F3)
- [ ] Tree view
- [ ] Compact view
- [ ] Group files by type
- [ ] Natural sort (numeric-aware)
- [ ] Embedded terminal

## Phase 10: Removable Media & Network

- [x] Auto-mount on insertion -- UDisks2 drive monitor (mount_async), drives enumerated in sidebar
- [x] Unmount -- UDisks2 unmount_async wired via left-click toggle and right-click context menu
- [ ] Format drive -- not implemented
- [ ] Disk image mounting (ISO) -- not implemented
- [ ] FTP/SFTP/SMB browsing -- not implemented
- [ ] Connect to Server dialog -- not implemented
- [ ] Cloud storage -- not implemented
- [ ] Network browsing (LAN) -- not implemented

## Phase 11: Extensions & Customization

(All not implemented. All keyboard shortcuts, toolbar buttons, and context menu items are hardcoded.)

- [ ] Plugin / extension system
- [ ] Service menus (.desktop actions)
- [ ] Scripts folder
- [ ] Customizable toolbar
- [ ] Configurable keyboard shortcuts
- [ ] File associations editor
- [ ] Show/hide sidebar, status bar, preview panel toggles

## Phase 12: Polish & Accessibility

- [x] Keyboard-only navigation -- full set: arrows, Return, Backspace, F2, Delete, Shift+Delete,
      Ctrl+C/X/V/A/Z, Alt+Arrows, F5, Ctrl+1/2, Ctrl+=/-, Ctrl+0, Ctrl+L, Tab/Shift+Tab cycling
- [x] Large text / high DPI scaling -- zoom_pct (50%-200%) scales all UI elements including fonts,
      icons, spacing, sidebar width
- [x] Smooth animations -- scroll_smooth_current/target with exponential easing via scroll_anim_start_ns
- [ ] High contrast mode -- not implemented
- [ ] Screen reader (Orca) support -- not implemented
- [ ] Full screen mode (F11) -- not implemented
- [ ] Focus indicator styling -- not implemented
- [ ] Smooth view transitions -- not implemented

---

## Keyboard Shortcut Reference

| Shortcut | Status |
|---|---|
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / Cut / Paste |
| Delete | Move to trash |
| Shift+Delete | Permanently delete |
| F2 | Rename |
| Ctrl+Shift+N | New folder |
| Ctrl+H | Toggle hidden files |
| Ctrl+A | Select all |
| Ctrl+Z | Undo |
| Ctrl+1 / Ctrl+2 | List view / Grid view |
| Ctrl+= / Ctrl+- / Ctrl+0 | Zoom in / Zoom out / Reset zoom |
| Alt+Up / Alt+Left / Alt+Right | Parent directory / Back / Forward |
| F5 | Reload current directory |
| Ctrl+L | Edit location bar |
| Tab / Shift+Tab | Cycle focus through interactive elements |
| Backspace | Navigate to parent |
| Return / KP_Enter | Open selected file/folder |

| Shortcut | Status |
|---|---|
| Ctrl+T / Ctrl+W / Ctrl+Tab | Tab management -- not implemented |
| Ctrl+F | Search bar -- not implemented |
| F3 | Split view -- not implemented |
| F4 | Open terminal -- not implemented |
| F11 | Full screen -- not implemented |
| Space | Preview -- not implemented |
| Ctrl+Shift+Z / Ctrl+Y | Redo -- not implemented |

---

## Architecture

- **Buffers**: double-buffered wl_shm buffers with busy tracking, pendingRedraw fallback when both busy
- **Event loop**: poll-based with timeout adaptation (0ms when thumbnails queued, 5s idle)
- **Clipboard**: custom clipboard service with cut/copy flags and undo-record push
- **Thumbnails**: two-tier cache -- in-memory LRU (64 MB) + WebP disk cache with stride-correct byte accounting
- **Icon loading**: IconCache with tray_icon lookup, prewarmed search directories, desktop-file icon extraction
- **Settings**: per-path folder settings saved to TOML, global defaults in separate file
- **Rendering**: cairo-based with Pango text layout, custom font/icon scaling from zoom_pct
- **Drag-and-drop**: data_device-based multi-source (file list + sidebar + favorites) with destination-aware
      drop, Ctrl key for copy-vs-move, cross-filesystem fallback, green drop-target highlight
