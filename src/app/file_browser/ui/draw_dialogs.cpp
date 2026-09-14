// draw_dialogs.cpp — create / confirm / conflict / password / compress / terminal chooser.
// Moved wholesale from ui/draw.cpp (byte-identical bodies).

#include "../app.hpp"
#include "../trace.hpp"
#include "../features/sidebar.hpp"
#include "../features/view_zoom.hpp"
#include "app/file_browser/features/thumb_pool.hpp"
#include "draw_helpers.hpp"
#include "draw_file_icons.hpp"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/common/icon_cache/icon_cache.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {

// Blit a monochrome SVG glyph tinted to the given color (mask paint).
static void blit_icon(cairo_t* cr, cairo_surface_t* svg, double x, double y,
                      double size, double r, double g, double b, double a = 1.0) {
  if (!svg) return;
  double sw = static_cast<double>(cairo_image_surface_get_width(svg));
  double sh = static_cast<double>(cairo_image_surface_get_height(svg));
  double sc = size / std::max(sw, sh);
  cairo_save(cr);
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_rectangle(cr, x, y, size, size);
  cairo_clip(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, sc, sc);
  cairo_mask_surface(cr, svg, 0, 0);
  cairo_restore(cr);
}

// ── create dialog ────────────────────────────────────────────────

void draw_create_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int dlg_w = 340;
  int dlg_h = 160;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Layered soft shadow (consistent with the redesigned menus)
  for (int s = 4; s >= 1; --s) {
    double a = 0.09 * (1.0 - s / 5.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, dlg_x + s, dlg_y + s, dlg_w, dlg_h, 10);
    cairo_fill(cr);
  }

  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, dlg_x, dlg_y, dlg_w, dlg_h, 10);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, dlg_x + 0.5, dlg_y + 0.5, dlg_w - 1, dlg_h - 1, 9.5);
  cairo_stroke(cr);

  // Header: icon + title (adapts to folder / document / template)
  const char* title = "New Folder";
  const char* placeholder = "Folder name";
  cairo_surface_t* hicon = app.icon_folder_svg;
  if (!app.create_is_folder) {
    title = app.create_template_src.empty() ? "New Document" : "New File from Template";
    placeholder = "File name";
    hicon = app.icon_file_text_svg;
  }
  cairo_save(cr);
  if (hicon) {
    double iw = static_cast<double>(cairo_image_surface_get_width(hicon));
    double ih = static_cast<double>(cairo_image_surface_get_height(hicon));
    double sc = 16.0 / std::max(iw, ih);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_rectangle(cr, dlg_x + 20, dlg_y + 14, 16, 16);
    cairo_clip(cr);
    cairo_translate(cr, dlg_x + 20, dlg_y + 14);
    cairo_scale(cr, sc, sc);
    cairo_mask_surface(cr, hicon, 0, 0);
  }
  cairo_restore(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, dlg_x + 44, dlg_y + 30);
  cairo_show_text(cr, title);

  int input_x = dlg_x + 20;
  int input_y = dlg_y + 50;
  int input_w = dlg_w - 40;
  int input_h = 34;
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
  draw_rounded_rect(cr, input_x, input_y, input_w, input_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, input_x + 0.5, input_y + 0.5, input_w - 1, input_h - 1, 5.5);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 14);

  // Draw selection highlight
  if (app.create_sel_start >= 0 && app.create_sel_start != app.create_sel_end) {
    int sel_a = std::min(app.create_sel_start, app.create_sel_end);
    int sel_b = std::max(app.create_sel_start, app.create_sel_end);
    std::string before_sel = app.create_buf.substr(0, static_cast<std::size_t>(sel_a));
    std::string sel_text = app.create_buf.substr(static_cast<std::size_t>(sel_a), static_cast<std::size_t>(sel_b - sel_a));
    cairo_text_extents_t te_before, te_sel;
    cairo_text_extents(cr, before_sel.c_str(), &te_before);
    cairo_text_extents(cr, sel_text.c_str(), &te_sel);
    double sel_x = input_x + 10 + te_before.width;
    double sel_y = input_y + 4;
    double sel_w = te_sel.width;
    double sel_h = static_cast<double>(input_h) - 8;
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
    cairo_rectangle(cr, sel_x, sel_y, sel_w, sel_h);
    cairo_fill(cr);
  }

  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, input_x + 10, input_y + input_h / 2 + 4);
  cairo_show_text(cr, app.create_buf.c_str());

  if (app.create_buf.empty()) {
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, input_x + 10, input_y + input_h / 2 + 4);
    cairo_show_text(cr, placeholder);
  } else {
    cairo_text_extents_t te;
    cairo_text_extents(cr, app.create_buf.substr(0, app.create_cursor_pos).c_str(), &te);
    int cx = input_x + 10 + static_cast<int>(te.width);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.7);
    cairo_rectangle(cr, cx, input_y + 6, 1, input_h - 12);
    cairo_fill(cr);
  }

  int btn_y = dlg_y + dlg_h - 50;
  int btn_w = 90;
  int btn_h = 32;
  int cancel_x = dlg_x + dlg_w - 220;
  int create_x = dlg_x + dlg_w - 110;

  // Cancel (secondary)
  double cancel_alpha = (app.create_hover_btn == 1) ? 0.75 : 0.55;
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, cancel_alpha);
  draw_rounded_rect(cr, cancel_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, cancel_x + 0.5, btn_y + 0.5, btn_w - 1, btn_h - 1, 5.5);
  cairo_stroke(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
  cairo_text_extents_t te;
  cairo_text_extents(cr, "Cancel", &te);
  cairo_move_to(cr, cancel_x + (btn_w - te.x_advance) / 2, btn_y + btn_h / 2 + te.height * 0.35);
  cairo_show_text(cr, "Cancel");

  // Create (primary)
  double create_alpha = (app.create_hover_btn == 0) ? 1.0 : 0.9;
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, create_alpha);
  draw_rounded_rect(cr, create_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 1.0);
  cairo_text_extents(cr, "Create", &te);
  cairo_move_to(cr, create_x + (btn_w - te.x_advance) / 2, btn_y + btn_h / 2 + te.height * 0.35);
  cairo_show_text(cr, "Create");
}

// ── confirm dialog ───────────────────────────────────────────────

void draw_confirm_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int dlg_w = 380;
  int dlg_h = 170;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card (fully opaque, layered soft shadow)
  draw_dialog_card(app, cr, dlg_x, dlg_y, dlg_w, dlg_h, 10);

  // Title (with trash icon)
  blit_icon(cr, app.trash_svg, dlg_x + 20, dlg_y + 14, 16,
            app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, dlg_x + 42, dlg_y + 31);
  cairo_show_text(cr, app.confirm_title.c_str());

  // File-type icon
  int icon_x = dlg_x + 20;
  int icon_y = dlg_y + 50;
  int icon_sz = 48;

  if (app.confirm_item_count == 1 && !app.confirm_preview_path.empty()) {
    FileType ft = FileType::File;
    auto dot = app.confirm_preview_path.rfind('.');
    if (dot != std::string::npos) {
      std::string ext = app.confirm_preview_path.substr(dot + 1);
      for (auto& c : ext) c = static_cast<char>(std::tolower(c));
      if (ext == "png" || ext == "jpg" || ext == "jpeg" ||
          ext == "gif" || ext == "bmp" || ext == "webp" ||
          ext == "svg" || ext == "avif" || ext == "tif" ||
          ext == "tiff" || ext == "psd" || ext == "xcf" ||
           ext == "ai" || ext == "eps" || ext == "af" || ext == "afphoto" ||
           ext == "afdesign" || ext == "afpub" || ext == "face" || ext == "icon")
        ft = FileType::Image;
      else if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "bz2" ||
               ext == "xz" || ext == "7z" || ext == "rar" || ext == "zst" ||
               ext == "zstd" || ext == "iso" || ext == "cab" || ext == "dmg")
        ft = FileType::Archive;
      else if (ext == "sh" || ext == "bin" || ext == "elf" || ext == "exe" ||
               ext == "desktop" || ext == "deb" || ext == "rpm" ||
               ext == "AppImage" || ext == "appimage" || ext == "flatpak" ||
               ext == "snap" || ext == "run" || ext == "msi")
        ft = FileType::Executable;
      else if (ext == "html" || ext == "htm" || ext == "xhtml" ||
               ext == "css" || ext == "php" || ext == "wasm")
        ft = FileType::Web;
      else if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mkd")
        ft = FileType::Markdown;
      else if (ext == "c" || ext == "cpp" || ext == "h" || ext == "hpp" ||
               ext == "py" || ext == "rs" || ext == "go" || ext == "java" ||
               ext == "js" || ext == "ts" || ext == "rb")
        ft = FileType::Code;
      else if (ext == "pdf" || ext == "doc" || ext == "docx" ||
               ext == "xls" || ext == "xlsx" || ext == "ppt" || ext == "pptx" ||
               ext == "odt" || ext == "ods" || ext == "odp" || ext == "rtf" ||
               ext == "epub" || ext == "djvu")
        ft = FileType::Document;
      else if (ext == "ttf" || ext == "otf" || ext == "woff" || ext == "woff2")
        ft = FileType::Font;
      else if (ext == "txt" || ext == "conf" || ext == "json" ||
               ext == "xml" || ext == "log" || ext == "yaml" ||
               ext == "yml" || ext == "toml" || ext == "ini" || ext == "cfg")
        ft = FileType::Text;
      else if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" ||
               ext == "m4a" || ext == "aac" || ext == "opus" || ext == "wma")
        ft = FileType::Audio;
      else if (ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov" ||
               ext == "webm")
        ft = FileType::Video;
    }
    const auto* ic = app.icons.tray_icon(icon_name_for_file_type(ft, &app.confirm_preview_path));
    if (ic && ic->surface) {
      double iw = static_cast<double>(ic->width);
      double ih = static_cast<double>(ic->height);
      if (iw > 0 && ih > 0) {
        double scale = icon_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, icon_x, icon_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, ic->surface,
                                 (icon_sz / scale - iw) / 2,
                                 (icon_sz / scale - ih) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
      }
    }
  }

  // Message text
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_text_extents_t te;
  int text_x = icon_x + icon_sz + 12;
  int text_max_w = dlg_x + dlg_w - 20 - text_x;
  std::string msg = app.confirm_message;
  if (cairo_text_extents(cr, msg.c_str(), &te),
      te.width > static_cast<double>(text_max_w)) {
    while (!msg.empty()) {
      msg.pop_back();
      cairo_text_extents(cr, (msg + "\u2026").c_str(), &te);
      if (te.width <= static_cast<double>(text_max_w)) {
        msg += "\u2026";
        break;
      }
    }
  }
  cairo_text_extents(cr, msg.c_str(), &te);
  int text_y = icon_y + icon_sz / 2 + static_cast<int>(te.height) / 2;
  cairo_move_to(cr, text_x, text_y);
  cairo_show_text(cr, msg.c_str());

  // Buttons
  int btn_y = dlg_y + dlg_h - 50;
  int btn_w = 90;
  int btn_h = 32;
  int cancel_x = dlg_x + dlg_w - 220;
  int delete_x = dlg_x + dlg_w - 110;

  bool cancel_hov = app.confirm_hover_btn == 0;
  bool delete_hov = app.confirm_hover_btn == 1;

  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b,
                        cancel_hov ? 0.75 : 0.55);
  draw_rounded_rect(cr, cancel_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.45);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, cancel_x + 0.5, btn_y + 0.5, btn_w - 1, btn_h - 1, 5.5);
  cairo_stroke(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cancel_x + btn_w / 2 - 20, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Cancel");

  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                        delete_hov ? 1.0 : 0.90);
  draw_rounded_rect(cr, delete_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_move_to(cr, delete_x + btn_w / 2 - 20, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Delete");
}

// ── Overwrite/merge conflict dialog (Dolphin-style) ─────────────

void draw_conflict_dialog(AppState& app, cairo_t* cr) {
  if (app.conflict_queue.empty()) return;
  const auto& c = app.conflict_queue.front();

  int w = app.width;
  int h = app.height;
  int dlg_w = 520;
  int dlg_h = 320;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.40);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card (fully opaque, layered soft shadow)
  draw_dialog_card(app, cr, dlg_x, dlg_y, dlg_w, dlg_h, 12);

  std::string name = fs::path(c.src).filename().string();
  bool merge = c.src_is_dir && c.dest_is_dir;

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  {
    std::string title = merge ? "Merge folder \"" + name + "\"?" :
                        (c.src_is_dir ? "Replace folder \"" + name + "\"?"
                                      : "Replace file \"" + name + "\"?");
    cairo_move_to(cr, dlg_x + 22, dlg_y + 32);
    cairo_show_text(cr, title.c_str());
  }

  // Subtitle
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
  {
    const char* sub = merge
        ? "The destination contains a folder with the same name. Files with the same name will be replaced."
        : (c.src_is_dir ? "The destination contains a file where the source has a folder."
                        : "The destination contains a file with the same name. Overwriting will replace its contents.");
    cairo_move_to(cr, dlg_x + 22, dlg_y + 52);
    cairo_show_text(cr, sub);
  }

  // ── Source vs Destination columns ──
  auto fmt_time = [](int64_t sec) {
    if (sec == 0) return std::string("\u2014");
    time_t t = static_cast<time_t>(sec);
    struct tm* tm_local = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_local);
    return std::string(buf);
  };
  auto fmt_size = [](uint64_t sz, bool is_dir) {
    if (is_dir) return std::string("Folder");
    char buf[64];
    double v = static_cast<double>(sz);
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int ui = 0;
    while (v >= 1024.0 && ui < 4) { v /= 1024.0; ++ui; }
    if (ui == 0) snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)sz);
    else snprintf(buf, sizeof(buf), "%.1f %s", v, units[ui]);
    return std::string(buf);
  };
  auto draw_column = [&](int x, int y, int col_w, const char* header,
                         const std::string& p, bool is_dir,
                         uint64_t sz, int64_t mtime) {
    // Header pill
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, header, &te);
    cairo_move_to(cr, x + (col_w - te.x_advance) / 2, y);
    cairo_show_text(cr, header);

    // Icon
    int icon_sz = 44;
    const auto* ic = app.icons.tray_icon(is_dir ? "folder"
                                        : icon_name_for_file_type(FileType::File, &p));
    if (ic && ic->surface) {
      double iw = static_cast<double>(ic->width);
      double ih = static_cast<double>(ic->height);
      if (iw > 0 && ih > 0) {
        double scale = icon_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, x + (col_w - icon_sz) / 2, y + 8);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, ic->surface,
                                 (icon_sz / scale - iw) / 2,
                                 (icon_sz / scale - ih) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
      }
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    std::string line1 = fmt_size(sz, is_dir);
    cairo_text_extents(cr, line1.c_str(), &te);
    cairo_move_to(cr, x + (col_w - te.x_advance) / 2, y + icon_sz + 28);
    cairo_show_text(cr, line1.c_str());

    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    std::string line2 = fmt_time(mtime);
    cairo_text_extents(cr, line2.c_str(), &te);
    cairo_move_to(cr, x + (col_w - te.x_advance) / 2, y + icon_sz + 46);
    cairo_show_text(cr, line2.c_str());
  };

  int col_top = dlg_y + 68;
  int col_w = 200;
  draw_column(dlg_x + 22, col_top, col_w, "SOURCE", c.src, c.src_is_dir, c.src_size, c.src_mtime);
  draw_column(dlg_x + dlg_w - 22 - col_w, col_top, col_w, "DESTINATION",
              c.dest, c.dest_is_dir, c.dest_size, c.dest_mtime);
  // Divider between columns
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, dlg_x + dlg_w / 2 + 0.5, col_top + 4);
  cairo_line_to(cr, dlg_x + dlg_w / 2 + 0.5, col_top + 92);
  cairo_stroke(cr);

  // ── Apply-to-all checkbox (only when more than one conflict remains) ──
  int check_y = dlg_y + dlg_h - 96;
  if (app.conflict_queue.size() > 1) {
    int box_sz = 16;
    int box_x = dlg_x + 22;
    app.conflict_check_rect[0] = box_x;
    app.conflict_check_rect[1] = check_y;
    app.conflict_check_rect[2] = box_sz + 220;
    app.conflict_check_rect[3] = box_sz;
    bool hov = app.conflict_check_hover ||
               (app.pointerX >= box_x && app.pointerX < box_x + box_sz + 220 &&
                app.pointerY >= check_y && app.pointerY < check_y + box_sz);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b,
                          hov ? 0.55 : 0.35);
    cairo_set_line_width(cr, 1.5);
    draw_rounded_rect(cr, box_x, check_y, box_sz, box_sz, 4);
    cairo_stroke(cr);
    if (app.conflict_apply_all) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
      draw_rounded_rect(cr, box_x, check_y, box_sz, box_sz, 4);
      cairo_fill(cr);
      // Checkmark
      cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
      cairo_set_line_width(cr, 2);
      cairo_move_to(cr, box_x + 4, check_y + 8);
      cairo_line_to(cr, box_x + 7, check_y + 11);
      cairo_line_to(cr, box_x + 12, check_y + 5);
      cairo_stroke(cr);
    }
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    std::string label = "Apply this action to all " +
                        std::to_string(app.conflict_queue.size() - 1) + " remaining";
    cairo_move_to(cr, box_x + box_sz + 8, check_y + 13);
    cairo_show_text(cr, label.c_str());
  } else {
    app.conflict_check_rect[0] = 0; app.conflict_check_rect[1] = -100;
    app.conflict_check_rect[2] = 0; app.conflict_check_rect[3] = 0;
  }

  // ── Buttons: Skip | Cancel | Overwrite(Merge) ──
  int btn_y = dlg_y + dlg_h - 54;
  int btn_h = 34;
  int btn_w = 104;
  int b2_x = dlg_x + dlg_w - 22 - btn_w;                    // Overwrite/Merge
  int b1_x = b2_x - 8 - btn_w;                              // Cancel
  int b0_x = b1_x - 8 - btn_w;                              // Skip
  double rects[3][4] = {
    {static_cast<double>(b0_x), static_cast<double>(btn_y), static_cast<double>(btn_w), static_cast<double>(btn_h)},
    {static_cast<double>(b1_x), static_cast<double>(btn_y), static_cast<double>(btn_w), static_cast<double>(btn_h)},
    {static_cast<double>(b2_x), static_cast<double>(btn_y), static_cast<double>(btn_w), static_cast<double>(btn_h)},
  };
  for (int b = 0; b < 3; ++b)
    for (int k = 0; k < 4; ++k) app.conflict_btn_rects[b][k] = rects[b][k];

  const char* labels[3] = {"Skip", "Cancel", merge ? "Merge" : "Overwrite"};
  int centers[3] = {b0_x, b1_x, b2_x};
  for (int b = 0; b < 3; ++b) {
    bool primary = (b == 2);
    bool hov = (app.pointerX >= rects[b][0] && app.pointerX < rects[b][0] + rects[b][2] &&
                app.pointerY >= rects[b][1] && app.pointerY < rects[b][1] + rects[b][3]);
    if (primary) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                            hov ? 1.0 : 0.90);
    } else {
      cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, hov ? 0.85 : 0.6);
    }
    draw_rounded_rect(cr, centers[b], btn_y, btn_w, btn_h, 6);
    cairo_fill(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, labels[b], &te);
    cairo_move_to(cr, centers[b] + (btn_w - te.x_advance) / 2, btn_y + btn_h / 2 + 4);
    cairo_show_text(cr, labels[b]);
  }
}

// ── password dialog ─────────────────────────────────────────────

void draw_password_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int card_w = 400;
  int card_h = 210;
  int cx = (w - card_w) / 2;
  int cy = (h - card_h) / 2;
  int pad = 24;
  int card_r = 14;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.40);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card (fully opaque, layered soft shadow)
  for (int s = 4; s >= 1; --s) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0.09 * (1.0 - s / 5.0));
    draw_rounded_rect(cr, cx + s, cy + s, card_w, card_h, card_r);
    cairo_fill(cr);
  }

  // Card background
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, cx, cy, card_w, card_h, card_r);
  cairo_fill_preserve(cr);

  // Card border
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  // Lock icon
  blit_icon(cr, app.lock_svg, cx + pad, cy + pad - 2, 16,
            app.accent_r, app.accent_g, app.accent_b, 0.9);

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cx + pad + 28, cy + pad + 15);
  cairo_show_text(cr, "Enter Password");

  // Subtitle with truncated archive name
  {
    std::string fname = fs::path(app.password_archive_path).filename().string();
    if (fname.size() > 42) fname = fname.substr(0, 39) + "\xE2\x80\xA6";
    std::string subtitle = "\"" + fname + "\" is password-protected";

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);

    cairo_text_extents_t te;
    cairo_text_extents(cr, subtitle.c_str(), &te);
    double max_w = static_cast<double>(card_w - pad * 2);
    if (te.width > max_w) {
      while (subtitle.size() > 10) {
        subtitle.pop_back();
        cairo_text_extents(cr, (subtitle + "\xE2\x80\xA6").c_str(), &te);
        if (te.width <= max_w) { subtitle += "\xE2\x80\xA6"; break; }
      }
    }
    cairo_move_to(cr, cx + pad, cy + pad + 36);
    cairo_show_text(cr, subtitle.c_str());
  }

  // Separator
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, cx + pad, cy + pad + 50);
  cairo_line_to(cr, cx + card_w - pad, cy + pad + 50);
  cairo_stroke(cr);

  // Input field
  int input_x = cx + pad;
  int input_y = cy + pad + 62;
  int input_w = card_w - pad * 2;
  int input_h = 36;
  int input_r = 8;

  // Input background
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.55);
  draw_rounded_rect(cr, input_x, input_y, input_w, input_h, input_r);
  cairo_fill(cr);

  // Input inner glassy rim
  {
    cairo_pattern_t* rim = cairo_pattern_create_linear(0, input_y, 0, input_y + input_h);
    cairo_pattern_add_color_stop_rgba(rim, 0.00, app.text_r, app.text_g, app.text_b, 0.14);
    cairo_pattern_add_color_stop_rgba(rim, 0.30, app.text_r, app.text_g, app.text_b, 0.06);
    cairo_pattern_add_color_stop_rgba(rim, 1.00, app.text_r, app.text_g, app.text_b, 0.04);
    cairo_set_source(cr, rim);
    cairo_set_line_width(cr, 1.0);
    draw_rounded_rect(cr, input_x + 1.5, input_y + 1.5, input_w - 3.0, input_h - 3.0,
                      static_cast<double>(input_r) - 0.5);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim);
  }

  // Password text (masked bullets)
  int text_y = input_y + input_h / 2;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 14);

  if (!app.password_buf.empty()) {
    std::string masked(app.password_buf.size(), '*');

    // Draw selection highlight
    if (app.password_sel_start >= 0 && app.password_sel_start != app.password_sel_end) {
      int sel_a = std::min(app.password_sel_start, app.password_sel_end);
      int sel_b = std::max(app.password_sel_start, app.password_sel_end);
      std::string before_sel(app.password_buf.begin(), app.password_buf.begin() + std::min(sel_a, static_cast<int>(app.password_buf.size())));
      std::string sel_masked(before_sel.size(), '*');
      std::string sel_part(sel_b - sel_a, '*');
      cairo_text_extents_t te_before, te_sel;
      cairo_text_extents(cr, sel_masked.c_str(), &te_before);
      cairo_text_extents(cr, sel_part.c_str(), &te_sel);
      double sel_x = input_x + 14 + te_before.width;
      double sel_y = static_cast<double>(input_y) + 4;
      double sel_w = te_sel.width;
      double sel_h = static_cast<double>(input_h) - 8;
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
      cairo_rectangle(cr, sel_x, sel_y, sel_w, sel_h);
      cairo_fill(cr);
    }

    cairo_text_extents_t te;
    cairo_text_extents(cr, masked.c_str(), &te);
    double tx = input_x + 14;
    double ty = text_y + te.height * 0.35;
    // Clip to input bounds
    cairo_save(cr);
    draw_rounded_rect(cr, input_x + 2, input_y + 2, input_w - 4, input_h - 4,
                      static_cast<double>(input_r) - 1);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.95);
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, masked.c_str());
    cairo_restore(cr);
  } else {
    // Placeholder
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.30);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "Password", &te);
    cairo_move_to(cr, input_x + 14, text_y + te.height * 0.35);
    cairo_show_text(cr, "Password");
  }

  // Cursor
  {
    std::string before_cursor(app.password_buf.begin(),
                              app.password_buf.begin() + std::min(app.password_cursor_pos,
                                                                   static_cast<int>(app.password_buf.size())));
    std::string masked_before(before_cursor.size(), '*');
    cairo_text_extents_t te;
    cairo_text_extents(cr, masked_before.c_str(), &te);
    int cur_x = input_x + 14 + static_cast<int>(te.width);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
    cairo_rectangle(cr, cur_x, input_y + 8, 1, input_h - 16);
    cairo_fill(cr);
  }

  // Buttons (pill-shaped)
  int btn_h = 32;
  int btn_w = 90;
  int btn_gap = 10;
  int btns_total = btn_w * 2 + btn_gap;
  int btns_x = cx + (card_w - btns_total) / 2;
  int btn_y = cy + card_h - pad - btn_h;

  // Cancel button
  {
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.42);
    draw_rounded_rect(cr, btns_x, btn_y, btn_w, btn_h, btn_h / 2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.40);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "Cancel", &te);
    cairo_move_to(cr, btns_x + (btn_w - te.x_advance) / 2,
                  btn_y + btn_h / 2 + te.height * 0.35);
    cairo_show_text(cr, "Cancel");
  }

  // Extract button (accent)
  {
    int ex_x = btns_x + btn_w + btn_gap;
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.15);
    draw_rounded_rect(cr, ex_x, btn_y, btn_w, btn_h, btn_h / 2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_text_extents_t te;
    cairo_text_extents(cr, "Extract", &te);
    cairo_move_to(cr, ex_x + (btn_w - te.x_advance) / 2,
                  btn_y + btn_h / 2 + te.height * 0.35);
    cairo_show_text(cr, "Extract");
  }
}

// ── compress dialog ──────────────────────────────────────────────

void draw_compress_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int dlg_w = 420;
  int dlg_h = 310;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card (fully opaque, layered soft shadow)
  draw_dialog_card(app, cr, dlg_x, dlg_y, dlg_w, dlg_h, 10);

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, dlg_x + 20, dlg_y + 28);
  cairo_show_text(cr, "Compress");

  // ── Format row ──
  static const char* fmt_labels[] = {"Zip", "Tar.gz", "Tar.bz2", "Tar.xz",
                                      "7z", "Rar", "Tar"};
  int content_x = dlg_x + 20;
  int content_y = dlg_y + 50;
  int fmt_w = 80;
  int fmt_h = 28;
  int fmt_gap = 8;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, content_x, content_y);
  cairo_show_text(cr, "Format");

  int fmy = content_y + 18;
  for (int i = 0; i < 4; ++i) {
    int fmx = content_x + i * (fmt_w + fmt_gap);
    bool avail = app.compress_format_available[i];
    double r = 6;
    if (i == app.compress_format) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, avail ? 0.85 : 0.30);
    } else if (i == app.compress_hover_format && avail) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.50);
    } else {
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    }
    draw_rounded_rect(cr, fmx, fmy, fmt_w, fmt_h, r);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, avail ? 1.0 : 0.35);
    cairo_move_to(cr, fmx + 6, fmy + fmt_h / 2 + 4);
    cairo_show_text(cr, fmt_labels[i]);
  }
  int fmy2 = fmy + fmt_h + fmt_gap;
  for (int i = 4; i < 7; ++i) {
    int fmx = content_x + (i - 4) * (fmt_w + fmt_gap);
    bool avail = app.compress_format_available[i];
    double r = 6;
    if (i == app.compress_format) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, avail ? 0.85 : 0.30);
    } else if (i == app.compress_hover_format && avail) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.50);
    } else {
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    }
    draw_rounded_rect(cr, fmx, fmy2, fmt_w, fmt_h, r);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, avail ? 1.0 : 0.35);
    cairo_move_to(cr, fmx + 6, fmy2 + fmt_h / 2 + 4);
    cairo_show_text(cr, fmt_labels[i]);
  }

  // ── Name row ──
  int name_y = fmy2 + fmt_h + 14;
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, content_x, name_y);
  cairo_show_text(cr, "Name");

  int input_x = content_x;
  int input_y = name_y + 18;
  int input_w = dlg_w - 40;
  int input_h = 32;
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
  draw_rounded_rect(cr, input_x, input_y, input_w, input_h, 6);
  cairo_fill(cr);

  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, input_x + 8, input_y + input_h / 2 + 4);
  cairo_show_text(cr, app.compress_name_buf.c_str());

  if (app.compress_name_buf.empty()) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.6);
    cairo_move_to(cr, input_x + 8, input_y + input_h / 2 + 4);
    cairo_show_text(cr, "Archive name");
  } else {
    cairo_text_extents_t te;
    cairo_text_extents(cr, app.compress_name_buf.substr(0, app.compress_name_cursor).c_str(), &te);
    int cx = input_x + 8 + static_cast<int>(te.width);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.7);
    cairo_rectangle(cr, cx, input_y + 6, 1, input_h - 12);
    cairo_fill(cr);
  }

  // ── Level row ──
  int lvl_y = input_y + input_h + 14;
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, content_x, lvl_y);
  cairo_show_text(cr, "Level");

  static const char* level_labels[] = {"Fastest", "Fast", "Normal", "Maximum", "Maximal"};
  static constexpr int kNumLevels = 5;
  static constexpr int kLevelValues[kNumLevels] = {0, 3, 6, 8, 9};
  int lvl_btn_x = content_x;
  int lvl_btn_y = lvl_y + 18;
  int lvl_btn_w = 68;
  int lvl_btn_h = 28;
  int lvl_gap = 8;
  for (int i = 0; i < kNumLevels; ++i) {
    int lx = lvl_btn_x + i * (lvl_btn_w + lvl_gap);
    double r = 6;
    if (kLevelValues[i] == app.compress_level) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
    } else if (i == app.compress_hover_level) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.50);
    } else {
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    }
    draw_rounded_rect(cr, lx, lvl_btn_y, lvl_btn_w, lvl_btn_h, r);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, lx + 4, lvl_btn_y + lvl_btn_h / 2 + 4);
    cairo_show_text(cr, level_labels[i]);
  }

  // ── Buttons ──
  int btn_y = dlg_y + dlg_h - 50;
  int btn_w = 90;
  int btn_h = 32;
  int cancel_x = dlg_x + dlg_w - 220;
  int compress_x = dlg_x + dlg_w - 110;

  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, 0.6);
  draw_rounded_rect(cr, cancel_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cancel_x + btn_w / 2 - 20, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Cancel");

  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.85);
  draw_rounded_rect(cr, compress_x, btn_y, btn_w, btn_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, compress_x + btn_w / 2 - 24, btn_y + btn_h / 2 + 4);
  cairo_show_text(cr, "Compress");

  // Hover highlight
  if (app.compress_hover_btn >= 0) {
    int hx = app.compress_hover_btn == 0 ? cancel_x : compress_x;
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.12);
    draw_rounded_rect(cr, hx, btn_y, btn_w, btn_h, 6);
    cairo_fill(cr);
  }
}

// ── terminal chooser dialog ──────────────────────────────────────

void draw_terminal_chooser(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;

  const int kPad = 20;
  const int kTopBarH = 44;
  const int kEntryH = 40;
  const int kBottomBarH = 52;
  const int kCardRad = 12;
  const int kMaxListH = 300;

  const int total = static_cast<int>(app.term_chooser_apps.size());
  const int max_visible = std::max(1, kMaxListH / kEntryH);
  const int visible = std::min(total, max_visible);
  const int list_h = visible * kEntryH;
  const int card_w = 400;
  const int card_h = kPad + kTopBarH + 8 + list_h + 8 + kBottomBarH + kPad;
  const int card_x = (w - card_w) / 2;
  const int card_y = (h - card_h) / 2;

  app.term_chooser_x = card_x;
  app.term_chooser_y = card_y;
  app.term_chooser_w = card_w;

  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card (fully opaque, layered soft shadow)
  draw_dialog_card(app, cr, card_x, card_y, card_w, card_h, kCardRad);

  const int close_x = card_x + card_w - kPad - 28;
  const int close_y = card_y + kPad - 4;
  {
    int cHov = (app.term_chooser_hover == -2) ? 1 : 0;
    double a = cHov ? 0.55 : 0.40;
    cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, a);
    draw_rounded_rect(cr, close_x, close_y, 28, 28, 6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, close_x + 9, close_y + 9);
    cairo_line_to(cr, close_x + 19, close_y + 19);
    cairo_move_to(cr, close_x + 19, close_y + 9);
    cairo_line_to(cr, close_x + 9, close_y + 19);
    cairo_stroke(cr);
  }

  // Title (with terminal icon)
  blit_icon(cr, app.monitor_svg, card_x + kPad, card_y + kPad - 2, 16,
            app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, card_x + kPad + 24, card_y + kPad + 15);
  cairo_show_text(cr, "Choose Terminal");

  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
  cairo_rectangle(cr, card_x, card_y + kPad + kTopBarH, card_w, 1);
  cairo_fill(cr);

  const int list_x = card_x + 12;
  const int list_y = card_y + kPad + kTopBarH + 8;
  const int list_w = card_w - 24;

  cairo_save(cr);
  cairo_rectangle(cr, list_x, list_y, list_w, list_h);
  cairo_clip(cr);

  const int start = app.term_chooser_scroll;
  const int end = std::min(start + visible, total);
  for (int i = start; i < end; ++i) {
    const int ey = list_y + (i - start) * kEntryH;
    const bool hov = (i == app.term_chooser_hover);

    if (hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
      cairo_rectangle(cr, list_x, ey, list_w, kEntryH);
      cairo_fill(cr);
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, list_x + 8, ey + kEntryH / 2 + 5);
    cairo_show_text(cr, app.term_chooser_apps[i].name.c_str());

    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.08);
    cairo_move_to(cr, list_x + 8, ey + kEntryH - 0.5);
    cairo_line_to(cr, list_x + list_w - 8, ey + kEntryH - 0.5);
    cairo_set_line_width(cr, 0.5);
    cairo_stroke(cr);
  }

  cairo_restore(cr);

  if (total > visible) {
    const double sbTrackH = list_h;
    const double sbH = std::max(6.0, sbTrackH * visible / static_cast<double>(total));
    const double sbMax = sbTrackH - sbH;
    const double frac = sbMax > 0
        ? static_cast<double>(app.term_chooser_scroll) /
              static_cast<double>(total - visible)
        : 0.0;
    const double sbY = list_y + frac * sbMax;
    const double sx = list_x + list_w - 8;
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.4);
    draw_rounded_rect(cr, sx, sbY, 4, sbH, 2);
    cairo_fill(cr);
  }
}

} // namespace eh::file_browser
