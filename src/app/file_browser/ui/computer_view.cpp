#include "../app.hpp"
#include "ui/design.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <mntent.h>

#include "services/udisks2/udisks2_drive_service.hpp"
#include "services/udisks2/drive_filter.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

// ── helpers ──────────────────────────────────────────────────────

static std::string home_dir_path() {
  if (auto* h = std::getenv("HOME")) return h;
  return "/root";
}

static std::string xdg_user_dir_path(const char* env, const char* fallback) {
  if (auto* e = std::getenv(env)) return e;
  return home_dir_path() + "/" + fallback;
}

static std::string format_size_binary(uint64_t bytes) {
  char buf[32];
  if (bytes == 0) return "0 B";
  const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  int unit = 0;
  double val = static_cast<double>(bytes);
  while (val >= 1024.0 && unit < 5) { val /= 1024.0; ++unit; }
  if (unit == 0)
    std::snprintf(buf, sizeof(buf), "%.0f %s", val, units[unit]);
  else if (val < 10.0)
    std::snprintf(buf, sizeof(buf), "%.1f %s", val, units[unit]);
  else
    std::snprintf(buf, sizeof(buf), "%.0f %s", val, units[unit]);
  return buf;
}

// ── refresh_computer ─────────────────────────────────────────────

void refresh_computer(AppState& app) {
  app.computer_items.clear();

  // ── Splitter: My Computer ──
  {
    ComputerItem split;
    split.shape = ComputerItem::ShapeType::Splitter;
    split.group = ComputerItem::Group::UserDirs;
    split.label = "My Computer";
    app.computer_items.push_back(split);
  }

  // ── Section 1: User Directories (Small items) ──
  auto add_user_dir = [&](const std::string& label, const std::string& icon,
                           const std::string& dir_path) {
    ComputerItem item;
    item.shape = ComputerItem::ShapeType::Small;
    item.group = ComputerItem::Group::UserDirs;
    item.label = label;
    item.icon_name = icon;
    item.path = dir_path;
    item.is_mounted = true;
    // Check if dir exists
    struct stat st;
    item.is_mounted = (stat(dir_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    std::error_code ec;
    auto space = fs::space(dir_path, ec);
    if (!ec) {
      item.total_bytes = space.capacity;
      item.used_bytes = space.capacity - space.available;
    }
    app.computer_items.push_back(item);
  };

  add_user_dir("Desktop",    "user-desktop",     xdg_user_dir_path("XDG_DESKTOP_DIR", "Desktop"));
  add_user_dir("Documents",  "folder-documents", xdg_user_dir_path("XDG_DOCUMENTS_DIR", "Documents"));
  add_user_dir("Downloads",  "folder-download",  xdg_user_dir_path("XDG_DOWNLOAD_DIR", "Downloads"));
  add_user_dir("Music",      "folder-music",     xdg_user_dir_path("XDG_MUSIC_DIR", "Music"));
  add_user_dir("Pictures",   "folder-pictures",  xdg_user_dir_path("XDG_PICTURES_DIR", "Pictures"));
  add_user_dir("Videos",     "folder-videos",    xdg_user_dir_path("XDG_VIDEOS_DIR", "Videos"));

  // ── Splitter: Disks ──
  {
    ComputerItem split;
    split.shape = ComputerItem::ShapeType::Splitter;
    split.group = ComputerItem::Group::Disks;
    split.label = "Disks";
    app.computer_items.push_back(split);
  }

  // ── Section 2: Drives (Large items with progress) ──
  auto& drives = eh::drives::UDisks2DriveService::instance();
  std::vector<std::string> seen_mounts;

  auto unescape_name = [](std::string s) {
    for (auto p = s.find("\\x20"); p != std::string::npos;
         p = s.find("\\x20", p + 1))
      s.replace(p, 4, " ");
    return s;
  };

  // Parse /proc/mounts for mounted block devices
  std::string root_dev;
  FILE* mtab = setmntent("/proc/mounts", "r");
  if (mtab) {
    struct mntent mnt_buf;
    char mnt_str_buf[4096];
    while (getmntent_r(mtab, &mnt_buf, mnt_str_buf, sizeof(mnt_str_buf))) {
      std::string dev = mnt_buf.mnt_fsname;
      std::string mp = mnt_buf.mnt_dir;
      if (dev.empty() || dev[0] != '/') continue;
      if (dev.find("/dev/") != 0) continue;
      if (drives::should_hide_drive(dev, mp, mnt_buf.mnt_type)) continue;
      // Skip if already seen
      if (std::find(seen_mounts.begin(), seen_mounts.end(), dev) != seen_mounts.end())
        continue;
      seen_mounts.push_back(dev);
      if (mp == "/") root_dev = dev;

      ComputerItem item;
      item.shape = ComputerItem::ShapeType::Large;
      item.group = ComputerItem::Group::Disks;
      item.path = mp;
      item.drive_id = dev;
      item.is_mounted = true;
      item.filesystem = mnt_buf.mnt_type;

      // Get filesystem label from /dev/disk/by-label/
      std::string dev_base = dev.substr(5); // strip "/dev/"
      std::error_code ec;
      for (auto& entry : fs::directory_iterator("/dev/disk/by-label/", ec)) {
        std::error_code ec2;
        std::string target = fs::read_symlink(entry.path(), ec2);
        // Basename equality: a substring match would let "sda1" claim the
        // label of "../../sda12".
        if (ec2) continue;
        auto slash = target.find_last_of('/');
        std::string link_base =
            (slash == std::string::npos) ? target : target.substr(slash + 1);
        if (link_base == dev_base) {
          item.label = unescape_name(entry.path().filename().string());
          item.is_user_label = true;
          break;
        }
      }
      // If no label on this device, try any partition on the same disk
      if (item.label.empty()) {
        std::string disk = eh::drives::disk_device_from_partition(dev);
        for (auto& entry : fs::directory_iterator("/dev/disk/by-label/", ec)) {
          std::error_code ec2;
          std::string target = fs::read_symlink(entry.path(), ec2).string();
          if (ec2 || target.empty()) continue;
          std::string link_dev;
          if (target.size() > 6 && target.substr(0, 6) == "../../")
            link_dev = "/dev/" + target.substr(6);
          else if (target.size() > 3 && target.substr(0, 3) == "../")
            link_dev = "/dev/" + target.substr(3);
          else
            link_dev = target;
          if (eh::drives::disk_device_from_partition(link_dev) == disk) {
            item.label = unescape_name(entry.path().filename().string());
            item.is_user_label = true;
            break;
          }
        }
      }
      // Try GPT partition label from /dev/disk/by-partlabel/
      if (item.label.empty()) {
        for (auto& entry : fs::directory_iterator("/dev/disk/by-partlabel/", ec)) {
          std::error_code ec2;
          std::string target = fs::read_symlink(entry.path(), ec2).string();
          if (ec2 || target.empty()) continue;
          std::string link_dev;
          if (target.size() > 6 && target.substr(0, 6) == "../../")
            link_dev = "/dev/" + target.substr(6);
          else if (target.size() > 3 && target.substr(0, 3) == "../")
            link_dev = "/dev/" + target.substr(3);
          else
            link_dev = target;
          if (link_dev == dev) {
            item.label = unescape_name(entry.path().filename().string());
            item.is_user_label = true;
            break;
          }
        }
      }
      if (item.label.empty()) {
        uint64_t size = eh::drives::get_device_size_bytes(dev);
        item.label = size > 0 ? eh::drives::format_device_size(size) : dev_base;
      }

      // Get space info
      struct statvfs vfs;
      if (statvfs(mp.c_str(), &vfs) == 0) {
        item.total_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_blocks;
        item.used_bytes = static_cast<uint64_t>(vfs.f_frsize) * (vfs.f_blocks - vfs.f_bfree);
      }

      // Determine icon
      item.icon_name = eh::drives::is_iso_loop_device(dev) ? "drive-optical" : "drive-harddisk";
      item.show_progress = true;
      app.computer_items.push_back(item);
    }
    endmntent(mtab);
  }

  // Exclude root device from unmounted scans (already shown above)
  std::vector<std::string> seen_devs = seen_mounts;
  if (!root_dev.empty()) seen_devs.push_back(root_dev);

  // Scan /dev/disk/by-label/ for unmounted labeled drives
  {
    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/dev/disk/by-label/", ec)) {
      std::error_code ec2;
      std::string target = fs::read_symlink(entry.path(), ec2).string();
      if (ec2 || target.empty()) continue;
      std::string dev;
      if (target.size() > 6 && target.substr(0, 6) == "../../")
        dev = "/dev/" + target.substr(6);
      else if (target.size() > 3 && target.substr(0, 3) == "../")
        dev = "/dev/" + target.substr(3);
      else
        dev = target;
      if (dev.size() < 5 || dev.substr(0, 5) != "/dev/") continue;
      if (drives::should_hide_drive(dev)) continue;
      // Attached-but-unmounted ISO loops never show (they vanish on unmount).
      if (drives::is_iso_loop_device(dev)) continue;
      if (std::find(seen_devs.begin(), seen_devs.end(), dev) != seen_devs.end())
        continue;
      seen_devs.push_back(dev);

      ComputerItem item;
      item.shape = ComputerItem::ShapeType::Large;
      item.group = ComputerItem::Group::Disks;
      item.label = unescape_name(entry.path().filename().string());
      item.drive_id = dev;
      item.is_mounted = false;
      item.is_user_label = true;
      item.icon_name = "drive-harddisk";
      app.computer_items.push_back(item);
    }
  }

  // Scan /dev/disk/by-partlabel/ for unmounted GPT drives
  {
    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/dev/disk/by-partlabel/", ec)) {
      std::error_code ec2;
      std::string target = fs::read_symlink(entry.path(), ec2).string();
      if (ec2 || target.empty()) continue;
      std::string dev;
      if (target.size() > 6 && target.substr(0, 6) == "../../")
        dev = "/dev/" + target.substr(6);
      else if (target.size() > 3 && target.substr(0, 3) == "../")
        dev = "/dev/" + target.substr(3);
      else
        dev = target;
      if (dev.size() < 5 || dev.substr(0, 5) != "/dev/") continue;
      std::string part_label = unescape_name(entry.path().filename().string());
      if (drives::should_hide_drive(dev, {}, {}, part_label)) continue;
      // Attached-but-unmounted ISO loops never show (they vanish on unmount).
      if (drives::is_iso_loop_device(dev)) continue;
      if (std::find(seen_devs.begin(), seen_devs.end(), dev) != seen_devs.end())
        continue;
      seen_devs.push_back(dev);

      ComputerItem item;
      item.shape = ComputerItem::ShapeType::Large;
      item.group = ComputerItem::Group::Disks;
      if (eh::drives::is_generic_partition_label(part_label)) {
        uint64_t size = eh::drives::get_device_size_bytes(dev);
        item.label = size > 0 ? eh::drives::format_device_size(size) : part_label;
      } else {
        item.label = part_label;
      }
      item.drive_id = dev;
      item.is_mounted = false;
      item.icon_name = "drive-harddisk";
      app.computer_items.push_back(item);
    }
  }

  // Add unmounted drives from UDisks2
  auto dinfo = drives.query_drives();
  for (auto& d : dinfo) {
    if (d.mounted) continue;
    if (d.device.empty()) continue;
    // Check if already in list
    bool found = false;
    for (auto& existing : app.computer_items) {
      if (existing.drive_id == d.device) { found = true; break; }
    }
    if (found) continue;

    ComputerItem item;
    item.shape = ComputerItem::ShapeType::Large;
    item.group = ComputerItem::Group::Disks;
    std::string display_label = d.label;
    if (display_label.empty() || display_label == d.device.substr(d.device.find_last_of('/') + 1) ||
        eh::drives::is_generic_partition_label(display_label)) {
      // Try partition label from by-partlabel
      std::string part_label;
      std::error_code ec2;
      for (auto& entry : fs::directory_iterator("/dev/disk/by-partlabel/", ec2)) {
        std::error_code ec3;
        std::string target = fs::read_symlink(entry.path(), ec3).string();
        if (ec3 || target.empty()) continue;
        std::string link_dev;
        if (target.size() > 6 && target.substr(0, 6) == "../../")
          link_dev = "/dev/" + target.substr(6);
        else if (target.size() > 3 && target.substr(0, 3) == "../")
          link_dev = "/dev/" + target.substr(3);
        else
          link_dev = target;
        if (link_dev == d.device) {
          std::string pl = unescape_name(entry.path().filename().string());
          if (!eh::drives::is_generic_partition_label(pl)) {
            part_label = pl;
          }
          break;
        }
      }
      if (!part_label.empty()) {
        item.label = part_label;
      } else {
        uint64_t size = eh::drives::get_device_size_bytes(d.device);
        item.label = size > 0 ? eh::drives::format_device_size(size) : "Local Disk";
      }
    } else {
      item.label = display_label;
    }
    item.drive_id = d.device;
    item.filesystem = d.id_type;
    item.path = d.mount_point;
    item.is_mounted = false;
    item.total_bytes = d.size;
    item.show_progress = d.size > 0;
    item.icon_name = "drive-harddisk";
    app.computer_items.push_back(item);
  }

  // ── Splitter: Network (placeholder) ──
  // No network items yet; section omitted.
}

// ── draw_computer_view ───────────────────────────────────────────

void draw_computer_view(AppState& app, cairo_t* cr, int content_x,
                        int content_y, int content_w, int view_h) {
  if (app.computer_needs_refresh) {
    refresh_computer(app);
    app.computer_needs_refresh = false;
  }

  double zf = app.zoom_pct / 100.0;
  const int kItemGap = static_cast<int>(16 * zf);
  const int kSmallW = static_cast<int>(140 * zf);
  const int kSmallH = static_cast<int>(140 * zf);
  const int kLargeW = static_cast<int>(300 * zf);
  const int kLargeH = static_cast<int>(96 * zf);
  const int kSplitterH = static_cast<int>(44 * zf);
  const int kCardRadius = static_cast<int>(16 * zf);
  const int kProgressBarH = static_cast<int>(6 * zf);
  const int kDriveGap = static_cast<int>(8 * zf);

  int items = static_cast<int>(app.computer_items.size());
  int y = content_y - app.computer_scroll_px;

  int total_small = 0, total_large = 0;
  for (auto& ci : app.computer_items) {
    if (ci.shape == ComputerItem::ShapeType::Small) ++total_small;
    if (ci.shape == ComputerItem::ShapeType::Large) ++total_large;
  }

  int small_per_row = std::max(1, (content_w - kItemGap) / (kSmallW + kItemGap));
  small_per_row = std::min(small_per_row, total_small);
  int small_total_w = small_per_row * kSmallW;
  int small_flex_gap = (content_w - small_total_w) / (small_per_row + 1);
  small_flex_gap = std::max(small_flex_gap, kItemGap);
  int small_offset_x = small_flex_gap;
  int large_cols = std::max(1, (content_w - kItemGap) / (kLargeW + kItemGap));
  large_cols = std::min(large_cols, total_large);
  int actual_large_w_base = (content_w - (large_cols + 1) * kItemGap) / large_cols;
  actual_large_w_base = std::min(actual_large_w_base, kLargeW);
  actual_large_w_base = std::max(actual_large_w_base, static_cast<int>(240 * zf));
  int total_drive_w = large_cols * actual_large_w_base;
  int flex_gap = (content_w - total_drive_w) / (large_cols + 1);
  flex_gap = std::max(flex_gap, kItemGap);
  int large_offset_x = flex_gap;

  int small_drawn = 0;
  int large_drawn = 0;

  for (int i = 0; i < items; ++i) {
    const auto& citem = app.computer_items[i];

    if (citem.shape == ComputerItem::ShapeType::Splitter) {
      app.hit_main.add(hui::Hit::view_row(i), content_x, y, content_w, kSplitterH);
      if (y + kSplitterH < content_y) { y += kSplitterH; continue; }
      if (y > content_y + view_h) break;
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 18.0 * zf);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_move_to(cr, content_x + static_cast<int>(20 * zf), y + kSplitterH - static_cast<int>(12 * zf));
      cairo_show_text(cr, citem.label.c_str());
      y += kSplitterH;
      continue;
    }

    if (citem.shape == ComputerItem::ShapeType::Small) {
      int col = small_drawn % small_per_row;
      int row = small_drawn / small_per_row;
      int cx = content_x + small_offset_x + col * (kSmallW + small_flex_gap);
      int cy = y + row * (kSmallH + kItemGap);

      app.hit_main.add(hui::Hit::view_row(i), cx, cy, kSmallW, kSmallH);
      if (cy + kSmallH < content_y) { ++small_drawn; continue; }
      if (cy > content_y + view_h) break;

      bool hovered = (i == app.computer_hover_idx);
      if (hovered) {
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.08);
        draw_rounded_rect(cr, cx, cy, kSmallW, kSmallH, kCardRadius);
        cairo_fill(cr);
      }

      // Icon area with colored circle background
      int icon_area_sz = static_cast<int>(84 * zf);
      int icon_area_x = cx + (kSmallW - icon_area_sz) / 2;
      int icon_area_y = cy + static_cast<int>(20 * zf);

      const auto* ic = app.icons.tray_icon(citem.icon_name.c_str());
      if (ic && ic->surface) {
        double iw = static_cast<double>(ic->width);
        double ih = static_cast<double>(ic->height);
        double scale = icon_area_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, icon_area_x, icon_area_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, ic->surface,
                                 (icon_area_sz / scale - iw) / 2,
                                 (icon_area_sz / scale - ih) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 28.0 * zf);
        cairo_text_extents_t te;
        char letter[2] = { citem.label.empty() ? '?' : citem.label[0], '\0' };
        cairo_text_extents(cr, letter, &te);
        cairo_move_to(cr, icon_area_x + (icon_area_sz - te.width) / 2,
                       icon_area_y + icon_area_sz / 2 + te.height / 2);
        cairo_show_text(cr, letter);
      }

      // Label centered below icon
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 12.0 * zf);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_text_extents_t te;
      int max_label_w = kSmallW - static_cast<int>(12 * zf);
      std::string display = hui::design::clip_end(cr, citem.label, max_label_w);
      cairo_text_extents(cr, display.c_str(), &te);
      cairo_move_to(cr, cx + (kSmallW - static_cast<int>(te.width)) / 2,
                     cy + kSmallH - static_cast<int>(18 * zf));
      cairo_show_text(cr, display.c_str());

      ++small_drawn;
      // Advance y past small rows once we finish the last item
      if (small_drawn >= total_small) {
        int last_row = (total_small - 1) / small_per_row;
        y = y + (last_row + 1) * (kSmallH + kItemGap);
      }
      continue;
    }

    if (citem.shape == ComputerItem::ShapeType::Large) {
      int col = large_drawn % large_cols;
      int actual_large_w = actual_large_w_base;
      int cx = content_x + large_offset_x + col * (actual_large_w + flex_gap);
      int cy = y;

      app.hit_main.add(hui::Hit::view_row(i), cx, cy, actual_large_w, kLargeH);
      if (cy + kLargeH < content_y) { ++large_drawn; continue; }
      if (cy > content_y + view_h) break;

      bool hovered = (i == app.computer_hover_idx);
      // Translucent card tinted with the wallpaper-derived accent, so the
      // drive cards harmonize with the background wallpaper behind the window.
      double tint = 0.35;
      double card_r = app.surface_r * (1.0 - tint) + app.accent_r * tint;
      double card_g = app.surface_g * (1.0 - tint) + app.accent_g * tint;
      double card_b = app.surface_b * (1.0 - tint) + app.accent_b * tint;
      cairo_set_source_rgba(cr, card_r, card_g, card_b, hovered ? 0.55 : 0.32);
      draw_rounded_rect(cr, cx, cy, actual_large_w, kLargeH, kCardRadius);
      cairo_fill(cr);
      if (hovered) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.5);
        cairo_set_line_width(cr, 1.2);
      } else {
        cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
        cairo_set_line_width(cr, 1);
      }
      draw_rounded_rect(cr, cx + 0.5, cy + 0.5, actual_large_w - 1, kLargeH - 1,
                        kCardRadius - 0.5);
      cairo_stroke(cr);

      // Device icon (left side, aligned with device name)
      int dev_icon_sz = static_cast<int>(36 * zf);
      int icon_x = cx + static_cast<int>(14 * zf);
      int icon_y = cy + static_cast<int>(10 * zf);
      const auto* ic = app.icons.tray_icon(citem.icon_name.c_str());
      if (ic && ic->surface) {
        double iw = static_cast<double>(ic->width);
        double ih = static_cast<double>(ic->height);
        double scale = dev_icon_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, icon_x, icon_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, ic->surface,
                                 (dev_icon_sz / scale - iw) / 2,
                                 (dev_icon_sz / scale - ih) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 22.0 * zf);
        cairo_text_extents_t te;
        char letter[2] = { citem.label.empty() ? '?' : citem.label[0], '\0' };
        cairo_text_extents(cr, letter, &te);
        cairo_move_to(cr, icon_x + (dev_icon_sz - te.width) / 2,
                       icon_y + dev_icon_sz / 2 + te.height / 2);
        cairo_show_text(cr, letter);
      }

      // Text area right of icon
      int text_x = cx + static_cast<int>(14 * zf) + dev_icon_sz + static_cast<int>(12 * zf);
      int text_w = actual_large_w - (text_x - cx) - static_cast<int>(14 * zf);

      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);

      // Device name
      double name_font_sz = citem.is_user_label ? 15.0 * zf : 13.0 * zf;
      cairo_set_font_size(cr, name_font_sz);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_text_extents_t te;
      std::string dev_label = hui::design::clip_end(cr, citem.label, text_w);
      cairo_move_to(cr, text_x, cy + static_cast<int>(24 * zf));
      cairo_show_text(cr, dev_label.c_str());

      // Second line: filesystem + device path
      cairo_set_font_size(cr, 10.0 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 1.0);
      if (!citem.filesystem.empty() || !citem.drive_id.empty()) {
        std::string subtitle;
        if (!citem.filesystem.empty())
          subtitle = citem.filesystem + " \u2014 ";
        std::string dev_display = citem.drive_id;
        auto pos = dev_display.rfind('/');
        if (pos != std::string::npos && pos + 1 < dev_display.size())
          dev_display = dev_display.substr(pos + 1);
        subtitle += dev_display;
        cairo_move_to(cr, text_x, cy + static_cast<int>(38 * zf));
        cairo_show_text(cr, subtitle.c_str());
      }
      if (!citem.is_mounted) {
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                               app.text_secondary_b, 0.5);
        cairo_move_to(cr, text_x, cy + static_cast<int>(52 * zf));
        cairo_show_text(cr, "Click to mount");
      }

      // Progress bar + usage text (bottom of card)
      if (citem.show_progress && citem.total_bytes > 0) {
        int pb_y = cy + kLargeH - static_cast<int>(16 * zf);
        int pb_w = actual_large_w - static_cast<int>(28 * zf);

        cairo_set_font_size(cr, 10.0 * zf);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                               app.text_secondary_b, 1.0);

        if (citem.is_mounted && citem.used_bytes > 0) {
          std::string usage = format_size_binary(citem.used_bytes) + " / " +
                              format_size_binary(citem.total_bytes);
          cairo_move_to(cr, cx + static_cast<int>(14 * zf), pb_y - static_cast<int>(5 * zf));
          cairo_show_text(cr, usage.c_str());

          double frac = std::min(1.0, static_cast<double>(citem.used_bytes) /
                                       static_cast<double>(citem.total_bytes));

          hui::design::bar(cr, app, cx + static_cast<int>(14 * zf), pb_y, pb_w, frac,
                      static_cast<double>(kProgressBarH));
        } else {
          std::string usage = format_size_binary(citem.total_bytes);
          cairo_move_to(cr, cx + static_cast<int>(14 * zf), pb_y - static_cast<int>(5 * zf));
          cairo_show_text(cr, usage.c_str());
        }
      }

      ++large_drawn;
      if (large_drawn % large_cols == 0 || large_drawn >= total_large) {
        y = cy + kLargeH + kDriveGap;
      }
      continue;
    }
  }

  int computed_h = y - content_y + app.computer_scroll_px;
  app.computer_content_h = std::max(computed_h, view_h);
}

// ── hit_test_computer ────────────────────────────────────────────

int hit_test_computer(AppState& app, int x, int y) {
  // Resolved through the retained hit registry (item rects stored during
  // paint); the per-pane layout recomputation is gone, which also fixes
  // split-view picking (the old code always used main-pane geometry).
  if (app.computer_items.empty()) return -1;
  const uint32_t hid = app.hit_main.query(x, y);
  if ((hid & hui::Hit::kGroupMask) != hui::Hit::kViewRow) return -1;
  int idx = static_cast<int>(hid & hui::Hit::kIndexMask);
  if (idx < 0 || idx >= static_cast<int>(app.computer_items.size())) return -1;
  return idx;
}

} // namespace eh::file_browser
