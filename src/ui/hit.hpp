#pragma once

// hui::HitRegistry — the single source of truth shared by paint and input.
// Canonical home of the registry (moved from app/file_browser/ui/).
//
// Background: widget toolkits deliver pointer events by hit-testing each
// widget's *allocation* — the rect assigned once per layout. The rule is
// absolute: input code never recomputes geometry; paint and input read the
// same retained rects, so resizing can never desync them.
//
// Rules for new UI:
//   1. If you paint something clickable, register it: hits.add(id, ...)
//      using the same x/y/w/h locals you paint with (or better: the rect
//      hui::place() wrote into your layout node).
//   2. If you handle a click, query: hits.query(x, y). Never recompute a
//      painted rect in input code.
//   3. Registration order is paint order; query returns the topmost
//      (last-registered) match, mirroring paint-on-top wins.
//
// Dependency-free (no cairo, no app state); see tests/test_hit_registry.cpp.

#include <cstdint>
#include <vector>

namespace hui {

namespace Hit {
inline constexpr uint32_t kNone = 0;

// Group bases. Query with (id & kGroupMask) to match a group; low 16 bits
// carry an index where applicable (64K rows survive the largest folders;
// every family owns a disjoint group nibble so ids can never alias).
inline constexpr uint32_t kTab = 0x10000;          // + tab index: tab body
inline constexpr uint32_t kTabClose = 0x20000;     // + tab index: close zone
inline constexpr uint32_t kSidebarRow = 0x30000;   // + location index
inline constexpr uint32_t kSidebarFavAdd = 0x38000; // "Add to Favorites" row
inline constexpr uint32_t kTopBar = 0x40000;       // + (pane << 8) + control
inline constexpr uint32_t kDialog = 0x50000;       // + (dialog << 8) + control
inline constexpr uint32_t kViewRow = 0x60000;      // + row index
inline constexpr uint32_t kViewArrow = 0x70000;    // + tree row index (expander)
inline constexpr uint32_t kMenu = 0x80000;         // + (menu << 10) + row

inline constexpr uint32_t kGroupMask = 0xF0000;
inline constexpr uint32_t kIndexMask = 0xFFFF;

inline constexpr uint32_t tab(int i) { return kTab + static_cast<uint32_t>(i); }
inline constexpr uint32_t menu(int m, int row) {
  return kMenu + (static_cast<uint32_t>(m) << 10) + (static_cast<uint32_t>(row) & 0x3FF);
}
// Decoders for menu IDs (kMenu occupies the high bits, so the menu number
// is NOT (hid >> 8) & 0xFF — subtract the group base first).
inline constexpr int menu_id(uint32_t hid) {
  return static_cast<int>((hid - kMenu) >> 10);
}
inline constexpr int menu_row(uint32_t hid) {
  return static_cast<int>(hid & 0x3FF);
}
// Menu numbers (stable; add new ones at the end).
inline constexpr int kMenuCtx = 1;
inline constexpr int kMenuCtxSub = 2;
inline constexpr int kMenuSort = 3;
inline constexpr int kMenuColumns = 4;
inline constexpr int kMenuFilter = 5;
inline constexpr int kMenuDrop = 6;
inline constexpr uint32_t kHeaderSeg = 0x6800;     // + segment (0 Name,1 Size,2 Date,3 Type)
inline constexpr uint32_t kHeaderDiv = 0x7000;     // + divider index (0..2)
inline constexpr uint32_t tab_close(int i) { return kTabClose + static_cast<uint32_t>(i); }
inline constexpr uint32_t sidebar_row(int i) { return kSidebarRow + static_cast<uint32_t>(i); }
inline constexpr uint32_t view_row(int i) { return kViewRow + static_cast<uint32_t>(i); }
inline constexpr uint32_t view_arrow(int i) { return kViewArrow + static_cast<uint32_t>(i); }
// Top-bar controls live in 0x40000..0x401FF per pane bit below.
inline constexpr uint32_t kPane1 = 0x01000000;
inline constexpr uint32_t topbar(int pane, int ctrl) {
  return kTopBar + (static_cast<uint32_t>(pane) << 8) + static_cast<uint32_t>(ctrl);
}
// Top-bar control numbers (stable; add new ones at the end).
inline constexpr int kTopNavBack = 1;
inline constexpr int kTopNavForward = 2;
inline constexpr int kTopNavUp = 3;
inline constexpr int kTopFoldToggle = 4;
inline constexpr int kTopSearchBtn = 5;
inline constexpr int kTopFolderSearchBtn = 6;
inline constexpr int kTopViewToggle = 7;
inline constexpr int kTopSortBtn = 8;
inline constexpr int kTopGear = 9;
inline constexpr int kTopDots = 10;
inline constexpr int kTopPathBar = 11;
inline constexpr int kTopSearchBar = 12;
inline constexpr int kTopFilterBtn = 13;
inline constexpr int kTopModeSeg = 14;
inline constexpr int kTopCaseToggle = 15;
inline constexpr int kTopLockToggle = 16;
inline constexpr int kTopPathText = 17;
// Dialog numbers (stable; add new ones at the end).
inline constexpr int kDlgCreate = 1;
inline constexpr int kDlgRename = 2;
inline constexpr int kDlgConfirm = 3;
inline constexpr int kDlgPassword = 4;
inline constexpr int kDlgCompress = 5;
inline constexpr int kDlgTerm = 6;
inline constexpr int kDlgOpenWith = 7;
inline constexpr int kDlgBatch = 8;
inline constexpr int kDlgConflict = 9;
inline constexpr int kDlgSettings = 10;
inline constexpr uint32_t dialog(int dlg, int ctrl) {
  // 10-bit control field: app-indexed rows (Open-With lists every desktop
  // entry) exceed 255 on app-heavy systems and must not wrap into the
  // next dialog number. Isolate the dialog with (hid & 0xFFFFC00).
  return kDialog + (static_cast<uint32_t>(dlg) << 10) + (static_cast<uint32_t>(ctrl) & 0x3FF);
}
inline constexpr int dialog_ctrl(uint32_t hid) {
  return static_cast<int>(hid & 0x3FF);
}
// Create-dialog controls.
inline constexpr int kCreateInput = 1;
inline constexpr int kCreateCancel = 2;
inline constexpr int kCreateOk = 3;
// Rename-dialog controls (same numbering).
inline constexpr int kRenameInput = 1;
inline constexpr int kRenameCancel = 2;
inline constexpr int kRenameOk = 3;
// Confirm-dialog controls (no input field).
inline constexpr int kConfirmCancel = 2;
inline constexpr int kConfirmDelete = 3;
// Password-dialog controls.
inline constexpr int kPasswordInput = 1;
inline constexpr int kPasswordCancel = 2;
inline constexpr int kPasswordExtract = 3;
// Compress-dialog controls: single input/buttons plus indexed chip rows.
inline constexpr int kCompressNameInput = 1;
inline constexpr int kCompressCancel = 2;
inline constexpr int kCompressOk = 3;
inline constexpr int kCompressFormatBase = 10; // +0..6
inline constexpr int kCompressLevelBase = 20;  // +0..4
inline constexpr int kCompressThreadBase = 30; // +thread option index
// Terminal-chooser controls: close + visible rows by absolute app index.
inline constexpr int kTermClose = 2;
inline constexpr int kTermRowBase = 10;
// Settings-window controls.
inline constexpr int kSettingsClose = 2;
inline constexpr int kSettingsTabBase = 4; // +0..2
inline constexpr int kSettingsOk = 7;
inline constexpr int kSettingsApply = 8;
inline constexpr int kSettingsCancel = 9;
inline constexpr int kSettingsZoomDown = 10;
inline constexpr int kSettingsZoomUp = 11;
inline constexpr int kSettingsZoomField = 12;
inline constexpr int kSettingsFoldersToggle = 13;
inline constexpr int kSettingsTermDrop = 14;
inline constexpr int kSettingsDropItemBase = 15; // +visible index
inline constexpr int kSettingsIndepToggle = 21;
inline constexpr int kSettingsSurfSlider = 22;
inline constexpr int kSettingsSideSlider = 23;
inline constexpr int kSettingsTopSlider = 24;
inline constexpr int kSettingsStatusSlider = 25;
inline constexpr int kSettingsPrevSlider = 26;
inline constexpr int kSettingsDlgSlider = 27;
inline constexpr int kSettingsPropsSlider = 28;
inline constexpr int kSettingsScaleSlider = 29;
inline constexpr int kSettingsMatugen = 30;
inline constexpr int kSettingsColorEng = 31;
// Conflict-dialog controls: checkbox + 3 buttons.
inline constexpr int kConflictCheck = 1;
inline constexpr int kConflictBtnBase = 10; // +0 Skip, +1 Cancel, +2 Overwrite/Merge
// Open-With rows by absolute app index (sections are layout-only).
inline constexpr int kOpenRowBase = 10;
// Batch-rename controls.
inline constexpr int kBatchTab0 = 4;
inline constexpr int kBatchTab1 = 5;
inline constexpr int kBatchTemplate = 6;
inline constexpr int kBatchFind = 7;
inline constexpr int kBatchReplace = 8;
inline constexpr int kBatchAdd = 9;
inline constexpr int kBatchAddItemBase = 10; // +0..2
inline constexpr int kBatchCancel = 2;
inline constexpr int kBatchOk = 3;
} // namespace Hit

struct HitRegion {
  uint32_t id = Hit::kNone;
  int x = 0, y = 0, w = 0, h = 0;
};

class HitRegistry {
 public:
  void clear() { regions_.clear(); }

  void add(uint32_t id, int x, int y, int w, int h) {
    if (id == Hit::kNone || w <= 0 || h <= 0) return;
    regions_.push_back(HitRegion{id, x, y, w, h});
  }

  void add(uint32_t id, const HitRegion& r) { add(id, r.x, r.y, r.w, r.h); }

  // Topmost (last-registered) region containing the point, or kNone.
  [[nodiscard]] uint32_t query(int x, int y) const {
    for (size_t i = regions_.size(); i-- > 0;) {
      const auto& r = regions_[i];
      if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
        return r.id;
    }
    return Hit::kNone;
  }

  [[nodiscard]] size_t size() const { return regions_.size(); }
  [[nodiscard]] const HitRegion& at(size_t i) const { return regions_[i]; }

  // Topmost region containing the point, or nullptr. Use when input needs
  // the rect itself (cursor offsets); otherwise prefer query().
  [[nodiscard]] const HitRegion* find(int x, int y) const {
    for (size_t i = regions_.size(); i-- > 0;) {
      const auto& r = regions_[i];
      if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
        return &r;
    }
    return nullptr;
  }

  // Topmost region with an exact ID, or nullptr. The pointer stays valid
  // until the next add()/clear().
  [[nodiscard]] const HitRegion* find_id(uint32_t id) const {
    for (size_t i = regions_.size(); i-- > 0;) {
      if (regions_[i].id == id) return &regions_[i];
    }
    return nullptr;
  }

 private:
  std::vector<HitRegion> regions_;
};

} // namespace hui
