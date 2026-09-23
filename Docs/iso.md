# ISO Mount — Right-click > Mount ISO for Horizon File Manager

Wayland-native file manager (`horizon-files`). Goal: right-click `.iso/.img/.udf` >
`Mount` / `Unmount`, matching Nautilus + Dolphin UX, via UDisks2 (no sudo).

## 1. Current Horizon architecture (verified in-tree)

### 1.1 Context menu — where to hook

* Types: `src/app/file_browser/app_types.hpp:649-740` — `enum class ContextMenuAction`
  (`MountDrive` at `:673`, `BrowseArchive` at `:679`, no `MountIso` yet),
  `struct ContextMenuItem { action, label, sub_items, data }`.
* Build: `src/app/file_browser/features/menu.cpp:453-717` `open_context_menu()`.
  Archive submenu `src/app/file_browser/features/menu.cpp:644-668` is the template:
  ```cpp
  // menu.cpp:661
  if (archive_path && is_archive_extension(*archive_path)) {
    archive_item.sub_items.push_back(menu_item(BrowseArchive,"Browse Archive"));
    archive_item.sub_items.push_back(menu_item(Extract,"Extract"));
  ```
* Dispatch: `src/app/file_browser/features/context_actions.cpp:67-138`
  -> `src/app/file_browser/features/context_actions_items.cpp:51-626`
  `execute_item_action()` switch. `BrowseArchive:451-459` = `fork()+execlp("horizon-archive",path)`.
* Draw/hit-test: `src/app/file_browser/ui/draw_contextmenu.cpp:177-315`,
  submenu via `src/app/file_browser/input/click_dialogs.cpp:58-109`.
* Entry points: `src/app/file_browser/input/click_views.cpp:450-467` (file),
  `:406-448` (sidebar Mount/UnmountDrive), `:364-395` (computer view).

### 1.2 Filetype gap

* `src/app/file_browser/features/filetype.cpp:231-323,377-605`:
  `.iso -> FileType::Archive`, `iso -> application/x-cd-image`,
  `mime_to_file_type:187 x-iso9660-image -> Archive`.
* BUT `src/app/file_browser/features/compress.cpp:215-225`
  `is_archive_extension()` only allows `.zip,.tar.gz,.tar.bz2,.tar.xz,.7z,.rar,.tar`.
  ISO never gets Browse/Extract. `.img` maps to nothing (falls to File/Executable).

### 1.3 Mount/volume today — drives only

* `src/services/udisks2/udisks2_drive_service.cpp:77-152`:
  sdbus-c++ system-bus `org.freedesktop.UDisks2.Filesystem.{Mount,Unmount}`
  + `udisksctl mount/unmount -b` fallback, `mount_async/unmount_async` on detached thread.
  `query_drives()` = `/proc/mounts` parse only (`:17-44`), no `GetManagedObjects`,
  `bind_signals(){}` empty, no `Loop.*`.
* Sidebar `src/app/file_browser/features/sidebar.cpp:675` + computer
  `src/app/file_browser/ui/computer_view.cpp:55`: parse `/proc/mounts`, resolve
  `/dev/disk/by-label|by-partlabel`, filter via
  `src/services/udisks2/drive_filter.hpp:212-247` `should_hide_drive()`.
  **Blocker:** `drive_filter.hpp:43-47` `is_hidden_device()` hides all `loop*`.
  ISO loop mounts must be exempted (check `Loop.BackingFile` or
  `/sys/block/loopN/loop/backing_file`).
* GIO (`gio-2.0`, `meson.build:29`) only used for `g_content_type_guess()`.
  No `GVolumeMonitor/GMount`. Archive viewer `src/archive_viewer/` reads ISO via
  libarchive (`get_archive_info:198 .iso->{ISO,none}`) for list/extract only.

## 2. UDisks2 API (authoritative, storaged.org/udisks 2.11)

System bus, `org.freedesktop.UDisks2`, manager `/org/freedesktop/UDisks2/Manager`.
Polkit `org.freedesktop.udisks2.loop-setup` + `filesystem-mount` = `allow_active:yes`
— no password for local active user. Never `sudo mount -o loop`.

```
Manager.LoopSetup(IN h fd, IN a{sv} options, OUT o loop_path)
  options: read-only:b=TRUE, offset:t, size:t, no-part-scan:b, auth.no_user_interaction:b
Filesystem.Mount(IN a{sv} options, OUT s mount_path)
  options: fstype:s (omit; autodetect iso9660/udf), options:s ("ro"), auth.no_user_interaction:b
Filesystem.Unmount(IN a{sv} options)   # {force:b}
Loop.Delete(IN a{sv} options)
Loop props: BackingFile:ay, Autoclear:b, SetupByUID:u
Filesystem props: MountPoints:aay
Block props: IdUsage="filesystem", IdType="iso9660|udf", Device:ay -> /dev/loopN
```

Flow:

1. `open(iso, O_RDONLY|O_CLOEXEC)` -> `LoopSetup(fd, {read-only:true})` -> `/org/.../block_devices/loopN`
2. Wait for `Filesystem` iface (udev probe 100ms–2s, poll or `InterfacesAdded`).
3. `Mount({})` -> `/run/media/$USER/LABEL` (fallback UUID/basename).
4. Dedup: scan `GetBlockDevices`/`GetManagedObjects`, match `BackingFile == canonical(iso)` -> reuse/reveal `MountPoints[0]`.
5. Cleanup: `Unmount({})` THEN `Loop.Delete({})`. Do not rely on `Autoclear` alone.

sdbus-c++ sketch (already a dep):

```cpp
auto conn = sdbus::createSystemBusConnection();
auto mgr = sdbus::createProxy(*conn, "org.freedesktop.UDisks2",
  "/org/freedesktop/UDisks2/Manager");
int fd = open(iso.c_str(), O_RDONLY|O_CLOEXEC);
sdbus::UnixFd sfd{fd};
std::map<std::string,sdbus::Variant> o{{"read-only", true}};
sdbus::ObjectPath loop;
mgr->callMethod("LoopSetup").onInterface("org.freedesktop.UDisks2.Manager")
   .withArguments(sfd, o).storeResultsTo(loop);
auto dev = sdbus::createProxy(*conn, "org.freedesktop.UDisks2", loop);
std::map<std::string,sdbus::Variant> mo; // empty = autodetect
std::string mnt;
dev->callMethod("Mount").onInterface("org.freedesktop.UDisks2.Filesystem")
   .withArguments(mo).storeResultsTo(mnt);
// unmount:
dev->callMethod("Unmount").onInterface("org.freedesktop.UDisks2.Filesystem")
   .withArguments(std::map<std::string,sdbus::Variant>{});
dev->callMethod("Delete").onInterface("org.freedesktop.UDisks2.Loop")
   .withArguments(std::map<std::string,sdbus::Variant>{});
```

CLI fallback (exact):

```bash
LOOPDEV=$(udisksctl loop-setup -r -f "/path/to.iso" | grep -oE '/dev/loop[0-9]+')
udisksctl mount -b "$LOOPDEV"      # "Mounted /dev/loop0 at /run/media/$USER/LABEL."
udisksctl unmount -b "$LOOPDEV"
udisksctl loop-delete -b "$LOOPDEV"
udisksctl info -b "$LOOPDEV"
```

Format allowlist: mount `.iso` (`application/x-cd-image`), `.img` raw/hybrid
(`application/x-raw-disk-image`, may expose `loop0p1` — mount partition),
`.udf` (`application/x-udf-image`). Do NOT mount `.bin/.cue,.nrg,.mdf/.mds`
— offer Extract/Convert (`bchunk,mdf2iso,nrg2iso,iat`) instead.

## 3. GNOME Files (Nautilus 51, GNOME 51 "A Coruña") — Sept 2026 state

Verified: GNOME 51 released 2026-09-16 (`release.gnome.org/51`), Files 51.0.1
released 2026-09-15 (`apps.gnome.org/Nautilus`). My earlier 46/47/48 refs were stale.

Files 51 changes (release notes + 51.alpha): drag counter badge for multi-drag,
smarter selection (copied files auto-selected, right-click empty no longer clears
selection), clearer file states (correct read-only/unreadable emblem with precedence),
non-blocking slow ops + faster reload, grouped notifications, notification categories
for mount/unmount operations. No new file-level `Mount` verb — ISO flow unchanged.

No file-level `Mount` verb. `.iso` goes through MIME default handler.

* Double-click/Open -> `gnome-disk-image-mounter.desktop` (verified
  `gio mime application/x-cd-image` and `application/vnd.efi.iso` both default to it).
* Right-click -> `Open With > Disk Image Mounter` (+ Writer, VLC alternates).
* Provider: `gnome-disk-utility 51` stable (was `46.1`, `51.beta 2026-07-30`):
  full GTK4/LibAdwaita port (off GTK3/libhandy), Rust rewrite of disk-image-mounter
  + restore dialog via `udisks-rs` (`9to5linux.com 2026-08-09` first look).
  `/usr/bin/gnome-disk-image-mounter`, `/usr/share/applications/gnome-disk-image-mounter.desktop`:
  ```ini
  Exec=gnome-disk-image-mounter %U
  MimeType=application/x-cd-image;application/x-raw-disk-image;application/x-raw-disk-image-xz-compressed;
  NoDisplay=true
  ```
  `*.iso` canonical MIME is `application/vnd.efi.iso` with alias `application/x-cd-image`,
  so the entry covers ISOs.
* What mounter does (`gnome-disk-image-mounter(1)`, source `src/disk-image-mounter/window.rs:mount()`):
  `LoopSetup(fd, {read-only:true})` ONLY (`-w/--writable` overrides).
  It does NOT call `Filesystem.Mount` — mount is left to `gvfs-udisks2-volume-monitor` +
  GNOME Shell automounter, then it polls `MountPoints` and launches Files at each mountpoint.
* Dialog (GTK4/libadwaita, Blueprint `gdu-image-mounter-window.blp`):
  `Open in Files` (read-only) / `Edit Contents in Files` (writable) /
  `Write to Drive` / `Inspect in Disks` / `Unmount Disk Image` (when attached).
  Status: `Already mounted`, `Compressed image files are not mountable`.
  Errors via `Adw.Toast`: `Failed to mount/unmount file`.
* Unmount: `Filesystem.Unmount({})` (+ `Encrypted.Lock` if LUKS) then `Loop.Delete({})`
  (`src/libgdu/gduutils.rs:unuse_data_iterate()`). Sidebar eject = GIO
  `GMount.unmount/eject` -> same UDisks2 calls. Nautilus never calls `mount(2)`/losetup.
* Mountpoint: `/run/media/$USER/$LABEL` (e.g. `/run/media/matt/SILENT HILL Townfall`).
* Disks 51 extras (same release): multi-keyfile + background unlock for LUKS,
  redesigned Benchmark + space-allocation bar + per-job spinner, job-tracking for
  resize/format/restore, direct `.xz` restore with progress, persisted mount options
  (`restore defaults`), Eject/Detach back, remembers window size, take-ownership action.

Sources: `release.gnome.org/51` (Files section), `apps.gnome.org/Nautilus` (51.0.1 2026-09-15),
`9to5linux.com/.../gnome-disks-...-gtk4-...-first-look` (2026-08-09),
`9to5linux.com/.../gnome-51-alpha-...` (2026-07-03, mount/unmount notification categories),
`github.com/GNOME/gnome-disk-utility` (`window.rs`, `gdu-image-mounter-window.blp`, `NEWS`),
`manpages.debian.org/.../gnome-disk-image-mounter.1`, `storaged.org/udisks/docs`.

## 4. KDE Dolphin (Plasma 6, Gear 24.x/25.x/26.x) — 2026 state

Plugin still present, not replaced. Repo `invent.kde.org/sdk/dolphin-plugins`
(mirror `github.com/KDE/dolphin-plugins`), dirs `mountiso/` + `mountedisooverlay/` on `master`.

* Type: `KAbstractFileItemActionPlugin` (`MountIsoAction`), NOT a servicemenu:
  `mountiso/mountisoaction.cpp/.h`, `mountisoaction.json` (`Name: "Mount ISO and disk images"`),
  `CMakeLists.txt`: `kcoreaddons_add_plugin(mountisoaction ... INSTALL_NAMESPACE "kf6/kfileitemaction")`.
  Toggle: Dolphin `Settings > Configure Dolphin > Context Menu > [x] Mount ISO and disk images`.
* Backend: Solid + UDisks2 D-Bus directly, no shell-out (`mountisoaction.cpp:mount()`):
  1. `open(file,O_RDONLY)` -> `QDBusUnixFileDescriptor(fd)`
  2. System-bus `Manager.LoopSetup(fd,{})` -> `.../block_devices/loopN`
     (equiv `udisksctl loop-setup -r -f image.iso`)
  3. Wait Solid `deviceAdded` (4x5s), lookup `Solid::StorageVolume` by UUID
  4. `StorageAccess::setup()` each (equiv `udisksctl mount -b /dev/loopN`)
  `unmount()`: `StorageAccess::teardown()` each then `Loop.Delete({})`
  (equiv `unmount -b ...; loop-delete -b ...`).
  State: `getDeviceFromBackingFile()` compares Solid `BackingFile` property.
* UX: top-level (not `Actions>` submenu, not `Open With`): `Mount` (`media-mount`) /
  `Unmount` (`media-eject`), mutually exclusive, single local-file selection only.
* MIME allowlist (`mountisoaction.cpp` + `.json` must match):
  `application/x-cd-image, application/x-raw-disk-image, application/vnd.efi.iso, application/vnd.efi.img`.
  `vnd.efi.*` added Nov 2023 (`aec8511a`, bug `bugs.kde.org/475659`) because
  `shared-mime-info>=2.3` prefers `vnd.efi.iso/.img` — menu vanished without it.
  No `.udf` handling — `.iso/.img` only.
* Partition-aware: mounts ALL `StorageAccess` devices sharing UUID, so `loop0p1` hybrids work.
* Mountpoint: `/run/media/$USER/LABEL` via Solid `Places > Devices`.
* Known gap: eject from Devices panel only teardowns (unmount), NOT `Loop.Delete` —
  loop lingers; full cleanup needs right-click ISO -> `Unmount`
  (`discuss.kde.org/t/mounting-iso-files-in-dolphin/42996`).
* Plasma 6 change: KF5->KF6/Qt6 port only; new companion `mountedisooverlay/`
  (2026, Kai Uwe Broulik): `KOverlayIconPlugin` -> `emblem-mounted/emblem-unmounted`
  badges on the `.iso` via `BackingFile` tracking.
* Releases: Gear `24.12.x, 25.04.0 (2025-04-17), 25.08.x, 25.12.x, 26.08.1 (2026-09-10)`.

Sources: `github.com/KDE/dolphin-plugins` (`mountisoaction.cpp/.json/.h`, `CMakeLists.txt`,
`mountedisooverlay.cpp`), `invent.kde.org/sdk/dolphin-plugins`,
`bugs.kde.org/475659`, `apps.kde.org/dolphin_plugins`, `wiki.archlinux.org/title/Udisks|Dolphin`.

## 5. Comparison + recommendation for Horizon

|  | GNOME Files 51 | Dolphin 25/26 (Plasma 6) | Horizon (proposed) |
|---|---|---|---|
| Menu | `Open With > Disk Image Mounter` (no Mount verb) | top-level `Mount`/`Unmount` toggle | top-level `Mount`/`Unmount` toggle (Dolphin-style, clearer than GNOME) |
| Backend | `LoopSetup(ro)` only, Shell automounts | `LoopSetup` + Solid mount | sdbus `LoopSetup(ro)` + `Mount` (Dolphin flow, no Shell dependency) |
| Mountpoint | `/run/media/$USER/LABEL` | same | same |
| Cleanup | `Unmount`+`Loop.Delete` | `teardown`+`Delete` (ISO menu only) | `Unmount`+`Delete`, always both |
| Formats | `x-cd-image,x-raw-disk-image(+xz)` | `x-cd-image,x-raw-disk-image,vnd.efi.iso/img` | `iso,img,udf` + all four MIMEs + `x-udf-image` |
| Overlay | none | `emblem-mounted` (2026) | future: ro badge/lock hint |

Recommended: Dolphin-style toggle + GNOME-style read-only default + explicit
`Unmount` on both ISO and mountpoint + notification with `Show in File Manager`
+ auto-reveal new tab at `mount_path` (both rivals reveal).

Concrete Horizon steps:

1. `app_types.hpp:679` add `MountIso, UnmountIso`.
2. `compress.hpp/cpp` add `is_iso_image()` (`.iso,.img,.udf` + MIME check).
3. `menu.cpp:661` extend filter, push `Mount`/`Unmount` toggle via `find_loop_for_file()`.
4. `context_actions_items.cpp:451` add cases -> `mount_iso_async()` reusing
   `mount_pending_drive_id/mount_navigate_drive_id` flow in `embed.cpp:1454,1548`.
5. `UDisks2DriveService.{hpp,cpp}` add `mount_iso/unmount_iso/find_loop_for_file`
   (Option 1 sdbus, Option 2 `udisksctl` fallback).
6. `drive_filter.hpp:43-47` exempt ISO loops from `is_hidden_device()`.
7. `bind_signals()` (`InterfacesAdded/Removed`, `PropertiesChanged:MountPoints`)
   -> `sidebar_needs_refresh/computer_needs_refresh`.

Test:
```bash
udisksctl loop-setup -r -f test.iso | grep -oE '/dev/loop[0-9]+'
udisksctl mount -b /dev/loopX; ls /run/media/$USER/
udisksctl unmount -b /dev/loopX; udisksctl loop-delete -b /dev/loopX
```
