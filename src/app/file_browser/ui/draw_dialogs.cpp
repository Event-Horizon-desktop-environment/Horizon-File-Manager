// draw_dialogs.cpp — create / confirm / conflict / password / compress / terminal chooser.
// Moved wholesale from ui/draw.cpp (byte-identical bodies).

#include "../app.hpp"
#include "../trace.hpp"
#include "../features/compress/compress.hpp"
#include "../features/sidebar/sidebar.hpp"
#include "../features/view_zoom/view_zoom.hpp"
#include "app/file_browser/features/thumbnails/thumb_pool.hpp"
#include "draw_helpers.hpp"
#include "ui/design.hpp"
#include "ui/layout.hpp"
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

  // Card (shared dialog chrome)
  draw_dialog_card(app, cr, dlg_x, dlg_y, dlg_w, dlg_h, 12);

  // Header: icon + title (adapts to folder / document / template)
  const char* title = "New Folder";
  const char* placeholder = "Folder name";
  cairo_surface_t* hicon = app.icon_folder_svg;
  if (!app.create_is_folder) {
    title = app.create_template_src.empty() ? "New Document" : "New File from Template";
    placeholder = "File name";
    hicon = app.icon_file_text_svg;
  }
  // hui layout: every rect below derives from placed nodes — paint, hits
  // and input share one geometry (see src/ui/). Pixel-identical to the
  // previous hand-computed layout.
  hui::Node col;
  col.kind = hui::Node::Kind::Column;
  hui::Node header;
  header.w = 300;
  header.h = 36;
  hui::Node input;
  input.w = 300;
  input.h = 34;
  hui::Node vspace;
  vspace.flex = 1;
  hui::Node btnrow;
  btnrow.kind = hui::Node::Kind::Row;
  btnrow.gap = 20;
  btnrow.h = 32;
  hui::Node bspace;
  bspace.flex = 1;
  hui::Node cancel;
  cancel.w = 90;
  cancel.h = 32;
  hui::Node create;
  create.w = 90;
  create.h = 32;
  btnrow.children.push_back(std::move(bspace));
  btnrow.children.push_back(std::move(cancel));
  btnrow.children.push_back(std::move(create));
  col.children.push_back(std::move(header));
  col.children.push_back(std::move(input));
  col.children.push_back(std::move(vspace));
  col.children.push_back(std::move(btnrow));
  hui::measure(col, 300);
  hui::place(col, dlg_x + 20, dlg_y + 14, 300, dlg_h - 32);

  const hui::Node& header_r = col.children[0];
  const hui::Node& input_r = col.children[1];
  const hui::Node& cancel_r = col.children[3].children[1];
  const hui::Node& create_r = col.children[3].children[2];

  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCreate, 0), dlg_x, dlg_y, dlg_w, dlg_h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCreate, hui::Hit::kCreateInput), input_r.x,
                   input_r.y, input_r.w, input_r.h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCreate, hui::Hit::kCreateCancel), cancel_r.x,
                   cancel_r.y, cancel_r.w, cancel_r.h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCreate, hui::Hit::kCreateOk), create_r.x,
                   create_r.y, create_r.w, create_r.h);

  // Header content (icon + title into the header node).
  cairo_save(cr);
  if (hicon) {
    double iw = static_cast<double>(cairo_image_surface_get_width(hicon));
    double ih = static_cast<double>(cairo_image_surface_get_height(hicon));
    double sc = 16.0 / std::max(iw, ih);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 1.0);
    cairo_rectangle(cr, header_r.x, header_r.y, 16, 16);
    cairo_clip(cr);
    cairo_translate(cr, header_r.x, header_r.y);
    cairo_scale(cr, sc, sc);
    cairo_mask_surface(cr, hicon, 0, 0);
  }
  cairo_restore(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, header_r.x + 24, header_r.y + 16);
  cairo_show_text(cr, title);

  int input_x = input_r.x;
  int input_y = input_r.y;
  int input_w = input_r.w;
  int input_h = input_r.h;
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
  draw_rounded_rect(cr, input_x, input_y, input_w, input_h, 8);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.45);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, input_x + 0.5, input_y + 0.5, input_w - 1, input_h - 1, 7.5);
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

  hui::design::button(cr, app, cancel_r.x, cancel_r.y, cancel_r.w, cancel_r.h, "Cancel", false,
                 app.create_hover_btn == 1);
  hui::design::button(cr, app, create_r.x, create_r.y, create_r.w, create_r.h, "Create", true,
                 app.create_hover_btn == 0);
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
  std::string msg = hui::design::clip_end(cr, app.confirm_message, text_max_w);
  cairo_text_extents(cr, msg.c_str(), &te);
  int text_y = icon_y + icon_sz / 2 + static_cast<int>(te.height) / 2;
  cairo_move_to(cr, text_x, text_y);
  cairo_show_text(cr, msg.c_str());

  // hui layout: buttons derive from placed nodes (single source for
  // paint, hits and input — see src/ui/).
  hui::Node btnrow;
  btnrow.kind = hui::Node::Kind::Row;
  btnrow.gap = 20;
  hui::Node cancel;
  cancel.w = 90;
  cancel.h = 32;
  hui::Node del;
  del.w = 90;
  del.h = 32;
  btnrow.children.push_back(std::move(cancel));
  btnrow.children.push_back(std::move(del));
  hui::measure(btnrow, 200);
  hui::place(btnrow, dlg_x + 160, dlg_y + dlg_h - 50, 200, 32);

  const hui::Node& cancel_r = btnrow.children[0];
  const hui::Node& delete_r = btnrow.children[1];

  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgConfirm, 0), dlg_x, dlg_y, dlg_w, dlg_h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgConfirm, hui::Hit::kConfirmCancel), cancel_r.x,
                   cancel_r.y, cancel_r.w, cancel_r.h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgConfirm, hui::Hit::kConfirmDelete), delete_r.x,
                   delete_r.y, delete_r.w, delete_r.h);

  // Buttons
  hui::design::button(cr, app, cancel_r.x, cancel_r.y, cancel_r.w, cancel_r.h, "Cancel", false,
                 app.confirm_hover_btn == 0);
  hui::design::button(cr, app, delete_r.x, delete_r.y, delete_r.w, delete_r.h, "Delete", true,
                 app.confirm_hover_btn == 1);
}

// ── Overwrite/merge conflict dialog ─────────────

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
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgConflict, 0), dlg_x, dlg_y, dlg_w, dlg_h);

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
    return hui::design::date_md(sec);
  };
  auto fmt_size = [](uint64_t sz, bool is_dir) {
    if (is_dir) return std::string("Folder");
    return hui::design::size_human(sz);
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
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgConflict, hui::Hit::kConflictCheck), box_x, check_y,
                     box_sz + 220, box_sz);
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
  for (int b = 0; b < 3; ++b)
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgConflict, hui::Hit::kConflictBtnBase + b),
                     static_cast<int>(rects[b][0]), static_cast<int>(rects[b][1]),
                     static_cast<int>(rects[b][2]), static_cast<int>(rects[b][3]));

  const char* labels[3] = {"Skip", "Cancel", merge ? "Merge" : "Overwrite"};
  int centers[3] = {b0_x, b1_x, b2_x};
  for (int b = 0; b < 3; ++b) {
    bool primary = (b == 2);
    bool hov = (app.pointerX >= rects[b][0] && app.pointerX < rects[b][0] + rects[b][2] &&
                app.pointerY >= rects[b][1] && app.pointerY < rects[b][1] + rects[b][3]);
    hui::design::button(cr, app, centers[b], btn_y, btn_w, btn_h, labels[b], primary, hov);
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

  // Card (shared dialog chrome)
  draw_dialog_card(app, cr, cx, cy, card_w, card_h, card_r);

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
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);

    double max_w = static_cast<double>(card_w - pad * 2);
    std::string subtitle =
        "\"" + hui::design::clip_end(cr, fname, max_w - 60) + "\" is password-protected";
    cairo_move_to(cr, cx + pad, cy + pad + 36);
    cairo_show_text(cr, subtitle.c_str());
  }

  // Separator
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, cx + pad, cy + pad + 50);
  cairo_line_to(cr, cx + card_w - pad, cy + pad + 50);
  cairo_stroke(cr);

  // hui layout: input + buttons derive from placed nodes (single source
  // for paint, hits and input — see src/ui/).
  hui::Node pw_input;
  pw_input.w = card_w - pad * 2;
  pw_input.h = 36;
  hui::measure(pw_input, card_w - pad * 2);
  hui::place(pw_input, cx + pad, cy + pad + 62, card_w - pad * 2, 36);

  hui::Node pw_btnrow;
  pw_btnrow.kind = hui::Node::Kind::Row;
  pw_btnrow.gap = 10;
  hui::Node pw_cancel;
  pw_cancel.w = 90;
  pw_cancel.h = 32;
  hui::Node pw_extract;
  pw_extract.w = 90;
  pw_extract.h = 32;
  pw_btnrow.children.push_back(std::move(pw_cancel));
  pw_btnrow.children.push_back(std::move(pw_extract));
  hui::measure(pw_btnrow, card_w);
  hui::place(pw_btnrow, cx + (card_w - 190) / 2, cy + card_h - pad - 32, 190, 32);

  const hui::Node& pw_cancel_r = pw_btnrow.children[0];
  const hui::Node& pw_extract_r = pw_btnrow.children[1];

  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgPassword, 0), cx, cy, card_w, card_h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgPassword, hui::Hit::kPasswordInput), pw_input.x,
                   pw_input.y, pw_input.w, pw_input.h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgPassword, hui::Hit::kPasswordCancel), pw_cancel_r.x,
                   pw_cancel_r.y, pw_cancel_r.w, pw_cancel_r.h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgPassword, hui::Hit::kPasswordExtract), pw_extract_r.x,
                   pw_extract_r.y, pw_extract_r.w, pw_extract_r.h);

  // Input field
  int input_x = pw_input.x;
  int input_y = pw_input.y;
  int input_w = pw_input.w;
  int input_h = pw_input.h;
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

  // Buttons
  hui::design::button(cr, app, pw_cancel_r.x, pw_cancel_r.y, pw_cancel_r.w, pw_cancel_r.h, "Cancel", false, false);
  hui::design::button(cr, app, pw_extract_r.x, pw_extract_r.y, pw_extract_r.w, pw_extract_r.h, "Extract", true, false);
}

// ── compress dialog ──────────────────────────────────────────────

void draw_compress_dialog(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int dlg_w = 420;
  int dlg_h = 372;
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
  // hui layout: every interactive rect derives from placed nodes (single
  // source for paint, hits and input — see src/ui/). Matches the
  // hand-computed geometry below pixel for pixel.
  const std::vector<int> thread_opts = compress_thread_options();
  int th_btn_w = compress_thread_btn_w(static_cast<int>(thread_opts.size()));
  hui::Node cmp_col;
  cmp_col.kind = hui::Node::Kind::Column;
  auto mkrow = [](int count, int cw, int ch, int gap) {
    hui::Node row;
    row.kind = hui::Node::Kind::Row;
    row.gap = gap;
    for (int i = 0; i < count; ++i) {
      hui::Node cell;
      cell.w = cw;
      cell.h = ch;
      row.children.push_back(std::move(cell));
    }
    return row;
  };
  hui::Node fmtlabel;
  fmtlabel.w = 380;
  fmtlabel.h = 18;
  hui::Node fmtrow1 = mkrow(4, fmt_w, fmt_h, fmt_gap);
  hui::Node fmtrow2 = mkrow(3, fmt_w, fmt_h, fmt_gap);
  hui::Node namelabel;
  namelabel.w = 380;
  namelabel.h = 32;
  hui::Node nameinput;
  nameinput.w = 380;
  nameinput.h = 32;
  hui::Node lvllabel;
  lvllabel.w = 380;
  lvllabel.h = 32;
  hui::Node lvlrow = mkrow(5, 68, 28, 8);
  hui::Node thlabel;
  thlabel.w = 380;
  thlabel.h = 30;
  hui::Node throw_ = mkrow(static_cast<int>(thread_opts.size()), th_btn_w, 28, 8);
  hui::Node vspace;
  vspace.flex = 1;
  hui::Node btnrow;
  btnrow.kind = hui::Node::Kind::Row;
  btnrow.gap = 20;
  hui::Node bspace;
  bspace.flex = 1;
  hui::Node cancel;
  cancel.w = 90;
  cancel.h = 32;
  hui::Node ok;
  ok.w = 90;
  ok.h = 32;
  btnrow.children.push_back(std::move(bspace));
  btnrow.children.push_back(std::move(cancel));
  btnrow.children.push_back(std::move(ok));
  cmp_col.children.push_back(std::move(fmtlabel));
  cmp_col.children.push_back(std::move(fmtrow1));
  cmp_col.children.push_back(std::move(fmtrow2));
  cmp_col.children.push_back(std::move(namelabel));
  cmp_col.children.push_back(std::move(nameinput));
  cmp_col.children.push_back(std::move(lvllabel));
  cmp_col.children.push_back(std::move(lvlrow));
  cmp_col.children.push_back(std::move(thlabel));
  cmp_col.children.push_back(std::move(throw_));
  cmp_col.children.push_back(std::move(vspace));
  cmp_col.children.push_back(std::move(btnrow));
  hui::measure(cmp_col, 380);
  cmp_col.children[1].h = 36; // chip-row bottom pad
  hui::place(cmp_col, content_x, content_y, 380, 304);

  const hui::Node& fmtrow1_r = cmp_col.children[1];
  const hui::Node& fmtrow2_r = cmp_col.children[2];
  const hui::Node& namelabel_r = cmp_col.children[3];
  const hui::Node& nameinput_r = cmp_col.children[4];
  const hui::Node& lvllabel_r = cmp_col.children[5];
  const hui::Node& lvlrow_r = cmp_col.children[6];
  const hui::Node& thlabel_r = cmp_col.children[7];
  const hui::Node& throw_r = cmp_col.children[8];
  const hui::Node& btnrow_r = cmp_col.children[10];
  const hui::Node& cancel_r = btnrow_r.children[1];
  const hui::Node& ok_r = btnrow_r.children[2];

  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCompress, 0), dlg_x, dlg_y, dlg_w, dlg_h);
  for (int i = 0; i < 4; ++i)
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCompress, hui::Hit::kCompressFormatBase + i),
                     fmtrow1_r.children[static_cast<size_t>(i)].x,
                     fmtrow1_r.children[static_cast<size_t>(i)].y, fmt_w, fmt_h);
  for (int i = 4; i < 7; ++i)
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCompress, hui::Hit::kCompressFormatBase + i),
                     fmtrow2_r.children[static_cast<size_t>(i - 4)].x,
                     fmtrow2_r.children[static_cast<size_t>(i - 4)].y, fmt_w, fmt_h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCompress, hui::Hit::kCompressNameInput), nameinput_r.x,
                   nameinput_r.y, nameinput_r.w, nameinput_r.h);
  for (int i = 0; i < 5; ++i)
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCompress, hui::Hit::kCompressLevelBase + i),
                     lvlrow_r.children[static_cast<size_t>(i)].x,
                     lvlrow_r.children[static_cast<size_t>(i)].y, 68, 28);
  for (size_t ti = 0; ti < thread_opts.size(); ++ti)
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCompress,
                                      hui::Hit::kCompressThreadBase + static_cast<int>(ti)),
                     throw_r.children[ti].x, throw_r.children[ti].y, th_btn_w, 28);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCompress, hui::Hit::kCompressCancel), cancel_r.x,
                   cancel_r.y, cancel_r.w, cancel_r.h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgCompress, hui::Hit::kCompressOk), ok_r.x,
                   ok_r.y, ok_r.w, ok_r.h);

  for (int i = 0; i < 4; ++i) {
    const hui::Node& cell = fmtrow1_r.children[static_cast<size_t>(i)];
    int fmx = cell.x;
    int fmy = cell.y;
    bool avail = app.compress_format_available[i];
    bool sel = (i == app.compress_format);
    bool hov = (i == app.compress_hover_format && avail);
    if (sel) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, avail ? 0.95 : 0.30);
    } else if (hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
    } else {
      hui::design::card_fill(cr, app, 0.55);
    }
    draw_rounded_rect(cr, fmx, fmy, fmt_w, fmt_h, 8);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, sel ? 1 : app.text_r, sel ? 1 : app.text_g,
                          sel ? 1 : app.text_b, avail ? (sel ? 0.98 : 1.0) : 0.35);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           sel ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_move_to(cr, fmx + 6, fmy + fmt_h / 2 + 4);
    cairo_show_text(cr, fmt_labels[i]);
  }
  for (int i = 4; i < 7; ++i) {
    const hui::Node& cell = fmtrow2_r.children[static_cast<size_t>(i - 4)];
    int fmx = cell.x;
    int fmy2 = cell.y;
    bool avail = app.compress_format_available[i];
    bool sel = (i == app.compress_format);
    bool hov = (i == app.compress_hover_format && avail);
    if (sel) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, avail ? 0.95 : 0.30);
    } else if (hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
    } else {
      hui::design::card_fill(cr, app, 0.55);
    }
    draw_rounded_rect(cr, fmx, fmy2, fmt_w, fmt_h, 8);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, sel ? 1 : app.text_r, sel ? 1 : app.text_g,
                          sel ? 1 : app.text_b, avail ? (sel ? 0.98 : 1.0) : 0.35);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           sel ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_move_to(cr, fmx + 6, fmy2 + fmt_h / 2 + 4);
    cairo_show_text(cr, fmt_labels[i]);
  }

  // ── Name row ──
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, namelabel_r.x, namelabel_r.y + 14);
  cairo_show_text(cr, "Name");

  int input_x = nameinput_r.x;
  int input_y = nameinput_r.y;
  int input_w = nameinput_r.w;
  int input_h = nameinput_r.h;
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
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, lvllabel_r.x, lvllabel_r.y + 14);
  cairo_show_text(cr, "Level");

  static const char* level_labels[] = {"Fastest", "Fast", "Normal", "Maximum", "Maximal"};
  static constexpr int kNumLevels = 5;
  static constexpr int kLevelValues[kNumLevels] = {0, 3, 6, 8, 9};
  for (int i = 0; i < kNumLevels; ++i) {
    const hui::Node& cell = lvlrow_r.children[static_cast<size_t>(i)];
    int lx = cell.x;
    int lvl_btn_y = cell.y;
    int lvl_btn_w = cell.w;
    int lvl_btn_h = cell.h;
    bool sel = (kLevelValues[i] == app.compress_level);
    bool hov = (i == app.compress_hover_level);
    if (sel) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.95);
    } else if (hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
    } else {
      hui::design::card_fill(cr, app, 0.55);
    }
    draw_rounded_rect(cr, lx, lvl_btn_y, lvl_btn_w, lvl_btn_h, 8);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, sel ? 1 : app.text_r, sel ? 1 : app.text_g,
                          sel ? 1 : app.text_b, sel ? 0.98 : 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           sel ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, lx + 4, lvl_btn_y + lvl_btn_h / 2 + 4);
    cairo_show_text(cr, level_labels[i]);
  }

  // ── Threads row ──
  cairo_set_font_size(cr, 12);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 1.0);
  cairo_move_to(cr, thlabel_r.x, thlabel_r.y + 12);
  cairo_show_text(cr, "Threads");

  for (size_t ti = 0; ti < thread_opts.size(); ++ti) {
    const hui::Node& cell = throw_r.children[ti];
    int tx = cell.x;
    int th_btn_y = cell.y;
    int th_btn_w = cell.w;
    int th_btn_h = cell.h;
    bool sel = (thread_opts[ti] == app.compress_threads);
    bool hov = (static_cast<int>(ti) == app.compress_hover_threads);
    if (sel) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.95);
    } else if (hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
    } else {
      hui::design::card_fill(cr, app, 0.55);
    }
    draw_rounded_rect(cr, tx, th_btn_y, th_btn_w, th_btn_h, 8);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, sel ? 1 : app.text_r, sel ? 1 : app.text_g,
                          sel ? 1 : app.text_b, sel ? 0.98 : 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           sel ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    char th_label[16];
    if (thread_opts[ti] == 0)
      snprintf(th_label, sizeof(th_label), "Auto");
    else
      snprintf(th_label, sizeof(th_label), "%d", thread_opts[ti]);
    cairo_text_extents_t th_te;
    cairo_text_extents(cr, th_label, &th_te);
    cairo_move_to(cr, tx + (th_btn_w - th_te.x_advance) / 2,
                  th_btn_y + th_btn_h / 2 + 4);
    cairo_show_text(cr, th_label);
  }

  // ── Buttons ──
  hui::design::button(cr, app, cancel_r.x, cancel_r.y, cancel_r.w, cancel_r.h, "Cancel", false,
                 app.compress_hover_btn == 0);
  hui::design::button(cr, app, ok_r.x, ok_r.y, ok_r.w, ok_r.h, "Compress", true,
                 app.compress_hover_btn == 1);
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

  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgTerm, 0), card_x, card_y, card_w, card_h);

  const int close_x = card_x + card_w - kPad - 28;
  const int close_y = card_y + kPad - 4;
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgTerm, hui::Hit::kTermClose),
                   close_x, close_y, 28, 28);
  {
    bool hov = (app.term_chooser_hover == -2);
    if (hov) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
      draw_rounded_rect(cr, close_x, close_y, 28, 28, 14);
      cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, hov ? 0.85 : 0.45);
    cairo_set_line_width(cr, 1.6);
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
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgTerm, hui::Hit::kTermRowBase + i),
                     list_x, ey, list_w, kEntryH);
    const bool hov = (i == app.term_chooser_hover);

    if (hov) {
      hui::design::row_hover(cr, app, list_x + 4, ey + 2, list_w - 8, kEntryH - 4);
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
