# Horizon-files Implementation Status

Feature inventory from GNOME Files (Nautilus) and KDE Dolphin tracking Horizon-files implementation.

---

## Done

- ✓ **Navigation** — Breadcrumb bar, editable location bar (Ctrl+L), arrow keys, zoom, smooth scroll, rubber-band selection, extended selection, column resizing, show hidden, per-folder view memory
- ✓ **Views** — Icon grid, list/details, sort by name/size/type/date
- ✓ **Tabs** — New/close/reorder, middle-click close, tab context menu
- ✓ **Sidebar** — Home, Desktop, XDG dirs, Trash, Root, drives/removable, mount/unmount/eject, bookmarks, drag-and-drop
- ✓ **File Ops** — Copy/cut/paste, rename, delete/trash, new folder, duplicate, symlink, copy path, undo/redo, conflict resolution, progress bar, batch rename, extract/compress
- ✓ **Properties** — Full dialog (Basic/Permissions/Image/Media tabs), chmod + exec toggle, owner/group, image metadata, audio/video metadata
- ✓ **Thumbnails** — Image, SVG, WebP, PDF, EPUB, and video thumbnails (progressive LRU 64MB cache, ffmpeg, poppler-glib, librsvg, libwebp, libarchive)
- ✓ **Context Menu** — Open with, cut/copy/paste, rename, trash, archive, symlink, copy path, properties, terminal, sort by, paste into, Favorites
- ✓ **Misc** — Status bar, keyboard-only nav, high DPI zoom, smooth animations, auto-mount, eject/remove

---

## Implementation Phases

### Phase 1: Core Interaction

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
      New Folder, Properties, Sort submenu, Copy Path, Open in Terminal, Open With submenu
- [x] Status bar -- item count, selection info, free disk space via statvfs

### Phase 2: Navigation & Views

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

### Phase 3: Tabs

- [x] Tabbed browsing -- new tab, close tab, next/prev tab
- [x] Ctrl+T / Ctrl+W / Ctrl+Tab / Ctrl+Shift+Tab
- [x] Tab reorder (drag tabs)
- [x] Middle-click to close tab
- [x] Tab context menu (close others, duplicate tab)

### Phase 4: Sidebar & Places

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
- [x] Drive hotplug detection -- UDisks2 InterfacesAdded/Removed/PropertiesChanged signal callback triggers `refresh_sidebar()`; `/dev/disk` mtime poll as fallback for non-UDisks2 systems
- [ ] Network locations
- [ ] Recent files
- [ ] Drive free space indicator in sidebar (status bar only)

### Phase 5: Advanced File Operations

- [x] Duplicate file (Ctrl+D / context menu)
- [x] Create symlink (context menu)
- [x] Copy path to clipboard (Ctrl+Shift+C / context menu)
- [x] Undo (Ctrl+Z) -- in-memory operation stack supporting PasteCopy, PasteCut, Rename, NewFolder,
      NewFile, MoveToTrash
- [x] Redo (Ctrl+Y / Ctrl+Shift+Z)
- [~] File conflict resolution -- auto-rename with " (n)" suffix, no interactive dialog
- [x] Copy/Move progress bar -- async ThreadPool, per-file progress, sidebar panel, cancel via Esc/button
- [x] Extract archive (zip/tar.gz via libarchive)
- [x] Compress / create archive (context menu → Compress)
- [x] Batch rename (multi-select F2) -- template mode with [Original filename] + numbering dropdown,
      and find/replace mode
- [ ] New file from template (context menu → New Document)

### Phase 6: Properties & Metadata

- [x] Properties dialog -- Nautilus-style custom dialog with Basic / Permissions / Image / Media tabs
- [x] Permissions display and editing -- combo dropdowns per Owner/Group/Other, 4 levels, chmod + executable toggle
- [x] Owner/group display (via getpwuid / getgrgid)
- [x] Image dimensions, megapixels, color space, bit depth, alpha, compression, DPI (identify + ffprobe)
- [x] Audio/video metadata: duration, container, codec, dimensions, framerate, bitrate, sample rate, channels (ffprobe)
- [~] MIME type display -- detected via xdg-mime, Type column shows FileType enum (hidden by default)
- [x] Folder item count -- recursive_directory_iterator for selected folder, displayed in status bar
- [ ] Disk usage visualization (text free space in status bar only)

### Phase 7: Search & Filter

- [x] Type-to-find — printable characters activate inline search when no dialog/input is active
- [x] Search bar (Ctrl+F) — toggle via Ctrl+F or search button, Escape clears & closes
- [x] Filter by name inline — name-only filter, persists in tab state, re-applied on dir reload
- [x] Search by file type / date / size — single "Filter" button in search bar opens a dropdown with expandable sections (Type▸/Size▸/Date▸); clicking a section header expands inline options; filtered via `matches_filter()` for both inline and recursive search; size/mtime populated via `stat()` for recursive search results; filters reset on search dismiss
- [x] Recursive subdirectory search — both local (current dir) and home-wide modes search recursively including hidden folders; GLib `g_content_type_guess` MIME-based file type detection for unknown extensions; XDG folder icons + .desktop icon parsing in results; SVG thumbnails via librsvg
- [ ] Saved searches (virtual folders)

### Phase 8: Preview & Thumbnails

- [x] Image thumbnails -- progressive lazy loading with LRU-backed in-memory cache (64 MB limit),
      WebP disk cache via cache_webp_path_for_source, max 16 decodes per frame + 200 per loop iteration
- [x] Video thumbnails -- ffmpeg frame extraction for .mp4, .webm, .mkv, .mov, .avi, async 6-thread worker
- [x] Thumbnail cache management -- in-memory LRU with stride-correct byte accounting, WebP disk cache
      with transparent/corrupt/oversize detection and automatic purge
- [~] Thumbnail size configuration -- controlled by zoom_pct (list: 24px × zf, grid: 48px × zf),
      no separate thumbnail size setting
- [x] PDF thumbnails — poppler-glib first-page render, sized to max_px, white page background
- [x] EPUB cover thumbnails — libarchive ZIP reader + stb_image, cover/front/image priority
- [x] SVG thumbnails — librsvg render to cairo surface (NanoSVG fallback)
- [x] WebP thumbnails — libwebp WebPDecodeRGBA for .webp files
- [x] File hover preview — 400ms hover delay, aspect-ratio-sized popup for images, picture frame style; PDF/EPUB white page background; SVG via librsvg; extension-preserving filename truncation
- [ ] Space/Enter preview (Sushi-style inline viewer)
- [ ] Information panel (F11) -- multi-tab: Preview + Properties + Terminal

### Phase 9: Advanced Views & Panels

(ViewMode enum supports only List and Grid.)

- [ ] Split/pane view (F3)
- [ ] Tree view
- [ ] Compact view (Dolphin-style)
- [ ] Group files by type toggle
- [ ] Natural sort (numeric-aware)
- [ ] Embedded terminal (Ctrl+Shift+F4, VTE widget)

### Phase 10: Removable Media & Network

- [x] Auto-mount on insertion -- UDisks2 drive monitor (mount_async), drives enumerated in sidebar
- [x] Eject / safely remove with verification
- [ ] Format drive dialog (mkfs wrapper)
- [ ] Disk image mounting (ISO)
- [ ] FTP/SFTP/SMB browsing -- virtual filesystem layer
- [ ] Connect to Server dialog (protocol:// path)
- [ ] Cloud storage -- Google Drive / Nextcloud (OAuth + API)
- [ ] LAN network browsing (discovery)

### Phase 11: Extensions & Customization

(All keyboard shortcuts, toolbar buttons, and context menu items are hardcoded.)

- [ ] Plugin / extension system -- dynamic library loading or script host
- [ ] Service menus -- `.desktop` file actions in context menu
- [ ] Scripts folder -- `~/.local/share/horizon-files/scripts/`
- [ ] Customizable toolbar -- add/remove/reorder buttons
- [ ] Configurable keyboard shortcuts
- [ ] File associations editor -- per-MIME-type default app
- [ ] Show/hide sidebar, status bar, preview panel (view menu toggles)

### Phase 12: Polish & Accessibility

- [x] Keyboard-only navigation -- arrows, Return, Backspace, F2, Delete, Shift+Delete,
      Ctrl+C/X/V/A/Z, Alt+Arrows, F5, Ctrl+1/2, Ctrl+=/-, Ctrl+0, Ctrl+L, Tab/Shift+Tab cycling
- [x] Large text / high DPI scaling -- zoom_pct (50%-200%) scales all UI including fonts,
      icons, spacing, sidebar width
- [x] Smooth animations -- scroll_smooth_current/target with exponential easing via scroll_anim_start_ns
- [ ] High contrast mode -- detection and styling
- [ ] Screen reader (Orca) support -- accessible labels
- [ ] Full screen mode (F11)
- [ ] Focus indicator styling
- [ ] Undo/Redo persistence across sessions
- [ ] File operation history log

---

## Nautilus Features (not planned)

- Desktop icons (removed 3.6, available via extension)
- Google Drive via GOA
- Tracker full-text search
- Sushi quick preview
- Scripts in `~/.local/share/nautilus/scripts/`
- Python extensions
- Folder color/emblems (removed 3.6)
- Brasero CD/DVD burning
- File Roller integration

## Dolphin Features (not planned)

- Split pane (F3)
- Embedded terminal (F4)
- Information panel (F11)
- Baloo indexing (tags, ratings, comments)
- KIO slaves for network protocols
- Service menus
- Customizable toolbar
- Configurable shortcuts
- Filter bar inline typing
- Folder size in properties
- ISO mounting plugin
- Git/Dropbox/Nextcloud plugins
- Open as Root (kio-admin)
- Actions submenu
- Group files toggle
- Compact view

---

## Keyboard Shortcut Reference

| Shortcut | Action | Status |
|---|---|---|
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / Cut / Paste | ✓ |
| Delete | Move to trash | ✓ |
| Shift+Delete | Permanently delete | ✓ |
| F2 | Rename | ✓ |
| Ctrl+Shift+N | New folder | ✓ |
| Ctrl+H | Toggle hidden files | ✓ |
| Ctrl+A | Select all | ✓ |
| Ctrl+Z | Undo | ✓ |
| Ctrl+Y / Ctrl+Shift+Z | Redo | ✓ |
| Ctrl+1 / Ctrl+2 | List view / Grid view | ✓ |
| Ctrl+= / Ctrl+- / Ctrl+0 | Zoom in / Zoom out / Reset zoom | ✓ |
| Alt+Up / Alt+Left / Alt+Right | Parent directory / Back / Forward | ✓ |
| F5 | Reload current directory | ✓ |
| Ctrl+L | Edit location bar | ✓ |
| Tab / Shift+Tab | Cycle focus through interactive elements | ✓ |
| Backspace | Navigate to parent | ✓ |
| Return / KP_Enter | Open selected file/folder | ✓ |
| Ctrl+T / Ctrl+W / Ctrl+Tab | Tab management | ✓ |
| Ctrl+Shift+C | Copy path | ✓ |
| Ctrl+D | Duplicate file | ✓ |
| Ctrl+T | New tab | ✓ |
| Ctrl+W | Close tab | ✓ |
| Ctrl+F | Search bar | ✓ |
| F3 | Split view | |
| F4 | Open terminal | |
| F11 | Full screen | |
| Space | Preview (Sushi) | |
| Alt+. | Show hidden (Dolphin) | |
| Ctrl+3 | Compact view (Dolphin) | |

---

## Architecture

- **Buffers**: double-buffered wl_shm buffers with busy tracking, pendingRedraw fallback when both busy
- **Event loop**: poll-based with timeout adaptation (0ms when thumbnails queued, 5s idle)
- **Clipboard**: custom clipboard service with cut/copy flags and undo-record push
- **Thumbnails**: two-tier cache -- in-memory LRU (64 MB) + WebP disk cache with stride-correct byte accounting; async video thumbnails via ffmpegthumbnailer subproject with 6-thread worker pool, condition variable wake, freedesktop.org-compliant PNG cache at ~/.cache/thumbnails/large/
- **Icon loading**: IconCache with tray_icon lookup, prewarmed search directories, desktop-file icon extraction
- **Settings**: per-path folder settings saved to TOML, global defaults in separate file
- **Rendering**: cairo-based with Pango text layout, custom font/icon scaling from zoom_pct
- **Drag-and-drop**: data_device-based multi-source (file list + sidebar + favorites) with destination-aware
      drop, Ctrl key for copy-vs-move, cross-filesystem fallback, green drop-target highlight

<!--
  [x] = done, [~] = partial, [ ] = not started
  Generated from GNOME Files (Nautilus) 47+ and KDE Dolphin 25.04+ documentation.
-->
