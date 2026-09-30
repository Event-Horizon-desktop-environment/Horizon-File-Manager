# Horizon File Manager — Feature Gap Specs

Seven features with the loudest cross-desktop demand (GNOME Files + Dolphin,
researched 2026) that Horizon still lacks. Each section: demand evidence, how
the reference implementations do it, and the concrete Horizon design.

Conventions used below: `ContextMenuAction` lives in
`src/app/file_browser/app_types.hpp`, handlers are `ctx_*` in
`features/context_actions/context_actions_regions.cpp`, menus are built in
`features/menu/menu.cpp`, persisted prefs live in `FileBrowserSettings`
(`src/config/shell_config.{hpp,cpp}`), per-row art goes through
`platform/common/icon_cache`.

---

## 1. Folder colors / emblems

**Demand.** The most visible Nautilus gap: multi-hundred-comment forum/Reddit
threads, Ubuntu Budgie shipping `folder-color` by default, openSUSE COPR
demand. Dolphin side: "color my folders" service menus + Properties → custom
icon are perennial tutorials.

**How others do it.**
- Nautilus `folder-color` / `nautilus-annotations`: stores a custom icon URI
  in GIO metadata (`gio set <dir> metadata::custom-icon file:///...`), plus
  emblems via `metadata::emblems [emblem-name]`. Icons themselves are
  recolored copies of the theme folder SVG.
- Dolphin: writes `Icon=folder-red` (or absolute path) into the folder's
  `.directory` file (freedesktop Desktop Entry `.directory` spec); custom
  per-dir view properties under `~/.local/share/dolphin/view_properties/`.
- Dropbox-style overlays: emblem layer composited bottom-right of the icon.

**Horizon design (no new deps).**
- Storage: sidecar-free — write `.directory` (`[Desktop Entry]\nIcon=...`)
  for cross-FM compat AND mirror to GIO metadata for Nautilus compat.
  Ship 10–12 pre-tinted folder SVGs in `assets/UI` (grey, red, orange,
  yellow, green, teal, blue, purple, pink + default).
- Menu: folder rows + sidebar + background get `Folder Color ▸` submenu
  (swatch grid) and `Emblems ▸` (starred, synced, locked, important…).
- Paint: `icon_cache` resolves folder art as
  `base(folder.svg) + tint? + emblem overlay?`; emblems also drawn for
  git/dropbox states (see §4, §6 — one overlay pipeline serves all three).
- Actions: `SetFolderColor`, `ClearFolderColor`, `TagToggle`-style
  `EmblemToggle` (reuse `ctx_tag_toggle` pattern; emblems stored as
  `metadata::emblems` string array via GIO, matching Nautilus).
- Accept: set color in Horizon → shows in Nautilus/Dolphin and vice versa.

## 2. Bulk rename

**Demand.** Dolphin's built-in rename + KRename are showcase features;
Thunar's bulk-rename tool (F2) isNama famous enough that Arch Wiki documents
bolting it onto Dolphin. Nautilus has nothing.

**How others do it.**
- Thunar modes: Insert/Overwrite text, Numbering (`%n` with start/step/pad),
  Remove characters, Search & Replace, Insert Date/Time, Audio tags.
- KRename: template tokens (`$`, `#`, `[find-replace]`), live preview list,
  undo via `.krename` log, collision handling (skip/overwrite/auto-number).
- Dolphin inline: F2 on multi-select renames sequentially with Tab/Up/Down.

**Horizon design (no new deps).**
- Trigger: multi-select (≥2) → `Actions ▸ Rename…` opens a dialog reusing the
  properties-window surface pattern (`create_props_window_impl` in
  `embed.cpp`): mode dropdown, pattern field, live preview list (old → new,
  collision rows highlighted red), Apply/Cancel.
- Modes v1: Numbering (`name_001.ext`, start/step/pad), Search & Replace
  (plain + regex via `<regex>`), Insert prefix/suffix, Change case.
- Engine: two-phase (validate all → rename all); on partial failure roll
  back completed renames; collisions auto-suffixed ` (2)` unless user picks
  skip. Reuse `request_fs_operation` progress plumbing for >100 files.
- Keyboard: F2 on multi-select jumps straight to the dialog.
- Accept: preview matches result byte-for-byte on a 1000-file fixture;
  undo of a completed batch restores originals.

## 3. One-click image convert / resize / rotate

**Demand.** `nautilus-image-converter` ("Resize Images…"/"Rotate Images…")
was dropped from Fedora F37+ (never ported to GTK4) and is still requested;
Debian still ships 0.4.0. ImageMagick one-liners (`mogrify -resize`, `mogrify
-rotate -auto-orient`) are the folk replacement.

**How others do it.** Dialog: preset sizes (320/640/800/1024/1280/1600 +
custom), append suffix vs in-place, JPEG quality slider, preserve EXIF +
mtime (`mogrify` preserves both by default; `convert -path` for copies).
Rotate: 90/270 + auto-orient by EXIF.

**Horizon design (no new deps — reuse linked codecs).**
- Menu: image selection (1..N, incl. mixed with non-images → filtered with
  toast count) gets `Convert / Resize…` top-level item + `Rotate ├ 90° / 270° /
  Auto-orient (EXIF)` submenu. Multi-select shares one dialog.
- Engine, native (not ImageMagick subprocess): decode via existing
  `load_image_thumbnail` full-res path (libjpeg IDCT, libpng, libwebp, stb),
  resize with the cairo bilinear scaler already in `image_preview.cpp`,
  encode: JPEG via libjpeg (quality 1–100, default 92, xmp/exif copy),
  PNG via stb_image_write or libpng, WebP via libwebp (already linked).
  Runs on the compress `ThreadPool` with the standard progress panel.
- Lossless JPEG rotate: `jpeg_transform` (libjpeg-turbo `jpegtran`
  equivalent) when dimensions allow, fallback to decode/re-encode.
- Naming: `<name>_resized.<ext>` / `<name>_rotated.<ext>`; never overwrite
  without confirm dialog.
- Accept: EXIF orientation + timestamps preserved; PSNR spot-check vs
  ImageMagick reference on the test corpus.

## 4. Git / VCS status badges + actions

**Demand.** `dolphin-plugins` Git integration is in every "must-have KDE
plugins" list; standalone `dolphin-git-overlayicon-plugin` exists because
demand exceeds the bundled one. Nautilus has no equivalent.

**How others do it.** Dolphin-overlays: per-directory repo root discovery
(walk up to `.git`), `git status --porcelain` (or libgit2
`git_status_list_new` / `git_status_file`) cached per mtime, emblem overlays:
modified (orange `*`), staged (green `+`), untracked (`?`), conflict (red),
ignored (greyed), current-branch label in status bar.

**Horizon design (optional `libgit2` dep, CLI fallback).**
- Dependency: `libgit2` (`required: false`, `-DEH_HAVE_LIBGIT2`); without it,
  degrade to `git status --porcelain=v1 -z --untracked-files` subprocess
  (same pattern as `udisksctl` fallbacks), throttled per directory mtime.
- Discovery: walk up max 8 levels for `.git`/`.hg`; cache root per tab.
- Paint: reuse the §1 emblem pipeline — modified/staged/untracked/conflict
  badges bottom-right; branch name in the status bar next to the path.
- Menu: repo-root rows get `Git ▸` submenu: `Commit…` (message dialog +
  `git commit`), `Pull`, `Push`, `Open in gitg`/`lazygit` if installed,
  `Copy branch name`. File rows: `Diff`, `Stage`, `Restore`.
- Settings: General-tab toggle "Git status badges" (default on when
  libgit2 present), respecting `.gitignore` (untracked-ignored = no badge).
- Accept: kernel-tree listing stays at 60fps (status cached, never on paint
  thread); badges match `git status` on a fixture repo with all 5 states.

## 5. Samba share creation

**Demand.** `nautilus-share`'s right-click → Sharing Options is still the
documented Ubuntu/Mint Windows-interop flow; `net usershare` error-255
threads recur yearly.

**How others do it.** `net usershare add <name> <abspath> [comment]
[acl] [guest_ok=y|n]` as the user (requires `sambashare` group +
`usershare owner only = false` for foreign paths); shares listed via
`net usershare list --long`; deletion via `net usershare delete <name>`.
Nautilus gates the menu on `smbd` presence and group membership.

**Horizon design (no new deps — `net` subprocess like `udisksctl`).**
- Menu: local folder rows get `Share ▸` submenu (or Properties-tab row):
  `Share via Samba…` opens a small dialog (share name prefilled, comment,
  Allow guests checkbox, read-only toggle) → runs `net usershare add`.
  Shared folders show a "shared" emblem (§1 pipeline) and `Stop Sharing`.
- Preflight: if `net`/`smbd` missing → toast "Install samba"; if user not
  in `sambashare` → toast with the exact `usermod -aG` command (copyable).
- Errors surfaced verbatim (error 255 SID/guest issues are common).
- Accept: share created from Horizon is browsable from Windows/macOS;
  `net usershare list` round-trips; stop-sharing removes it.

## 6. Cloud sync status badges

**Demand.** Dropbox emblem threads ("overlays missing after update") recur on
every distro; Nextcloud/ownCloud desktop clients ship Nautilus/Dolphin
overlay plugins for the same reason. Horizon browses Drive/Nextcloud but
shows no sync state.

**How others do it.** Providers expose per-file state out-of-band and the FM
paints emblems: Dropbox `dropbox.py filestatus <path>` (up to date /
syncing / unsyncable / error), Nextcloud via the client socket
(`~/.config/Nextcloud/socket` + `RETRIEVE_FILE_STATUS`), Syncthing via REST
` /rest/db/status`. Overlays: ✓ synced, ⟳ syncing, ⚠ error, ⊘ ignored.

**Horizon design (no new deps).**
- Provider probes, cheapest first, cached 5s per directory:
  1. Dropbox: `dropbox.py filestatus` if the daemon dir (`~/Dropbox`)
     contains the path.
  2. Nextcloud: socket `STRING_GET` status query if
     `~/.config/Nextcloud/sync-exclude.lst` sibling socket exists.
  3. Syncthing: `GET /rest/db/status?folder=…` with API key from
     `~/.config/syncthing/config.xml` (localhost only).
  4. Native Drive remote (`drive:` URIs): all-synced (server truth) except
     failed uploads tracked in-app.
- Paint: same §1 emblem pipeline; syncing rows get a progress tint.
- Settings: per-provider toggles under Accounts; everything off when no
  client detected (zero-cost otherwise).
- Accept: pausing Dropbox flips emblems to paused within 5s; 10k-file
  Dropbox folder keeps 60fps (per-dir cache, never per-frame subprocess).

## 7. Shortcut-assignable custom actions

**Demand.** Dolphin 26.04's headline: shortcuts for nearly any menu/plugin
action — "closed a highly-requested feature". AskUbuntu/Reddit workarounds
(xdotool) predate it by a decade.

**How others do it.** Dolphin 26.04 registers every menu + service-menu
action in the global shortcut editor (`kglobalaccel`/KActionCollection);
service menus are `.desktop` files with `Actions=` + per-action `Exec=`,
`Icon=`, `MimeType=`.

**Horizon design (no new deps).**
- Registry: every `ContextMenuAction` + discovered Services action gets a
  stable id (`"open-in-terminal"`, `"service:<desktop-id>/<action>"`).
  New `key_shortcuts.cpp` map `action_id → key chord`, persisted as
  `shortcuts.<id> = "Ctrl+Shift+X"` in the file-browser TOML.
- UI: Settings → new Shortcuts tab listing all ids with click-to-rebind
  (reuse the key-dialog capture from `key_dialog.cpp`), conflict
  highlighting, Reset to defaults.
- Scope v1: Services submenu + the 10 most-used builtins (terminal, copy
  path, new tab/window, reload, properties, rename, compress, checksums,
  compare, disk usage). Multi-modifier chords via existing key dispatch.
- Accept: assign Ctrl+Shift+T to a custom service action → fires on
  selection with zero menu interaction; conflicts flagged, never double-bound.

---

## Build order (cheapest first)

1. §1 colors/emblems (overlay pipeline unlocks §4 + §6).
2. §3 image convert (codecs already linked).
3. §7 action registry + shortcuts (small, high visibility).
4. §2 bulk rename (dialog-heavy).
5. §4 git badges (libgit2 optional dep + new docs rows).
6. §5 samba sharing (`net usershare` runtime dep note).
7. §6 cloud badges (three provider probes).
