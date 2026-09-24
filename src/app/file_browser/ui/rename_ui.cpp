#include "../app.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <string>

#include "draw_helpers.hpp"
#include "ui/design.hpp"
#include "ui/layout.hpp"

namespace eh::file_browser {

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

void draw_rename_ui(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int dlg_w = 400;
  int dlg_h = 190;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card (shared dialog chrome)
  draw_dialog_card(app, cr, dlg_x, dlg_y, dlg_w, dlg_h, 12);

  // Header: edit icon + title
  blit_icon(cr, app.edit_svg, dlg_x + 20, dlg_y + 14, 16,
            app.text_secondary_r, app.text_secondary_g, app.text_secondary_b);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, dlg_x + 44, dlg_y + 30);
  cairo_show_text(cr, "Rename");

  auto slash = app.rename_ui_entry_path.rfind('/');
  std::string dir_str = (slash != std::string::npos)
    ? app.rename_ui_entry_path.substr(0, slash)
    : "";
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11);
  dir_str = hui::design::clip_middle(cr, dir_str, dlg_w - 90);
  blit_icon(cr, app.icon_folder_svg, dlg_x + 22, dlg_y + 41, 13,
            app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.7);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.45);
  cairo_move_to(cr, dlg_x + 42, dlg_y + 50);
  cairo_show_text(cr, dir_str.c_str());

  // hui layout: paint, hits and input share one geometry (see src/ui/).
  // Buttons sit on symmetric 24px margins.
  hui::Node col;
  col.kind = hui::Node::Kind::Column;
  hui::Node input;
  input.w = 352;
  input.h = 36;
  hui::Node vspace;
  vspace.flex = 1;
  hui::Node btnrow;
  btnrow.kind = hui::Node::Kind::Row;
  btnrow.gap = 20;
  btnrow.h = 34;
  hui::Node bspace;
  bspace.flex = 1;
  hui::Node cancel;
  cancel.w = 90;
  cancel.h = 34;
  hui::Node rename;
  rename.w = 90;
  rename.h = 34;
  btnrow.children.push_back(std::move(bspace));
  btnrow.children.push_back(std::move(cancel));
  btnrow.children.push_back(std::move(rename));
  col.children.push_back(std::move(input));
  col.children.push_back(std::move(vspace));
  col.children.push_back(std::move(btnrow));
  hui::measure(col, 352);
  hui::place(col, dlg_x + 24, dlg_y + 64, 352, dlg_h - 82);

  const hui::Node& input_r = col.children[0];
  const hui::Node& cancel_r = col.children[2].children[1];
  const hui::Node& rename_r = col.children[2].children[2];

  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgRename, 0), dlg_x, dlg_y, dlg_w, dlg_h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgRename, hui::Hit::kRenameInput), input_r.x,
                   input_r.y, input_r.w, input_r.h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgRename, hui::Hit::kRenameCancel), cancel_r.x,
                   cancel_r.y, cancel_r.w, cancel_r.h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgRename, hui::Hit::kRenameOk), rename_r.x,
                   rename_r.y, rename_r.w, rename_r.h);

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

  cairo_set_font_size(cr, 14);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);

  // Draw selection highlight (x_advance, not width: width ignores trailing
  // spaces so the highlight/cursor would stick before a space).
  if (app.rename_ui_sel_start >= 0 && app.rename_ui_sel_start != app.rename_ui_sel_end) {
    int n = static_cast<int>(app.rename_ui_buf.size());
    int sel_a = std::min(app.rename_ui_sel_start, app.rename_ui_sel_end);
    int sel_b = std::max(app.rename_ui_sel_start, app.rename_ui_sel_end);
    if (sel_a < 0) sel_a = 0;
    if (sel_b > n) sel_b = n;
    if (sel_a < sel_b) {
      std::string before_sel = app.rename_ui_buf.substr(0, static_cast<std::size_t>(sel_a));
      std::string sel_text = app.rename_ui_buf.substr(static_cast<std::size_t>(sel_a), static_cast<std::size_t>(sel_b - sel_a));
      cairo_text_extents_t te_before, te_sel;
      cairo_text_extents(cr, before_sel.c_str(), &te_before);
      cairo_text_extents(cr, sel_text.c_str(), &te_sel);
      double sel_x = input_x + 12 + te_before.x_advance;
      double sel_y = static_cast<double>(input_y) + 4;
      double sel_w = te_sel.x_advance;
      double sel_h = static_cast<double>(input_h) - 8;
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
      cairo_rectangle(cr, sel_x, sel_y, sel_w, sel_h);
      cairo_fill(cr);
    }
  }

  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, input_x + 12, input_y + input_h / 2 + 5);
  cairo_show_text(cr, app.rename_ui_buf.c_str());

  if (app.rename_ui_buf.empty()) {
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.35);
    cairo_move_to(cr, input_x + 12, input_y + input_h / 2 + 5);
    cairo_show_text(cr, "New name");
  }

  // Cursor: always drawn (even on an empty buffer) and measured with
  // x_advance so a trailing space advances it. A space counts as a
  // character when stepping with the arrow keys.
  {
    int n = static_cast<int>(app.rename_ui_buf.size());
    int cur = app.rename_ui_cursor_pos;
    if (cur < 0) cur = 0;
    if (cur > n) cur = n;
    // Snap mid-codepoint positions back so cairo never sees partial UTF-8.
    while (cur > 0 && cur < n &&
           (static_cast<unsigned char>(app.rename_ui_buf[static_cast<std::size_t>(cur)]) & 0xC0) == 0x80)
      --cur;
    cairo_text_extents_t te;
    cairo_text_extents(cr, app.rename_ui_buf.substr(0, static_cast<std::size_t>(cur)).c_str(), &te);
    int cx = input_x + 12 + static_cast<int>(te.x_advance);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.65);
    cairo_rectangle(cr, cx, input_y + 8, 1, input_h - 16);
    cairo_fill(cr);
  }

  hui::design::button(cr, app, cancel_r.x, cancel_r.y, cancel_r.w, cancel_r.h, "Cancel", false,
                 app.rename_ui_hover_btn == 1);
  hui::design::button(cr, app, rename_r.x, rename_r.y, rename_r.w, rename_r.h, "Rename", true,
                 app.rename_ui_hover_btn == 0);
}

void draw_batch_rename(AppState& app, cairo_t* cr) {
  int w = app.width;
  int h = app.height;
  int n = static_cast<int>(app.batch_rename_entries.size());

  // ── Compute preview names ──
  bool is_template = (app.batch_rename_mode == 0);
  if (is_template) {
    int counter = 1;
    for (auto& e : app.batch_rename_entries) {
      std::string base = e.old_name;
      if (!e.ext.empty() && e.old_name.size() > e.ext.size())
        base = e.old_name.substr(0, e.old_name.size() - e.ext.size());
      std::string result = app.batch_rename_template;
      // Replace [Original filename] with base
      for (auto p = result.find("[Original filename]"); p != std::string::npos;
           p = result.find("[Original filename]", p + base.size()))
        result.replace(p, 19, base);
      // Replace [1] with counter (1-digit)
      std::string c1 = std::to_string(counter);
      for (auto p = result.find("[1]"); p != std::string::npos;
           p = result.find("[1]", p + c1.size()))
        result.replace(p, 3, c1);
      // Replace [01] with 2-digit counter
      char buf2[16];
      std::snprintf(buf2, sizeof(buf2), "%02d", counter);
      std::string c2(buf2);
      for (auto p = result.find("[01]"); p != std::string::npos;
           p = result.find("[01]", p + c2.size()))
        result.replace(p, 4, c2);
      // Replace [001] with 3-digit counter
      char buf3[16];
      std::snprintf(buf3, sizeof(buf3), "%03d", counter);
      std::string c3(buf3);
      for (auto p = result.find("[001]"); p != std::string::npos;
           p = result.find("[001]", p + c3.size()))
        result.replace(p, 5, c3);
      e.new_name = result + e.ext;
      ++counter;
    }
  } else {
    for (auto& e : app.batch_rename_entries) {
      std::string n = e.old_name;
      if (!app.batch_rename_find.empty()) {
        std::size_t pos = 0;
        while ((pos = n.find(app.batch_rename_find, pos)) != std::string::npos) {
          n.replace(pos, app.batch_rename_find.size(), app.batch_rename_replace);
          pos += app.batch_rename_replace.size();
        }
      }
      e.new_name = n;
    }
  }

  // ── Layout ──
  int dlg_w = 540;
  int list_h = std::min(n * 28 + 4, 280) + 4;
  int input_area_h = is_template ? 70 : 80;
  int dlg_h = 24 + 28 + input_area_h + list_h + 56;
  int dlg_x = (w - dlg_w) / 2;
  int dlg_y = (h - dlg_h) / 2;

  // Backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // Card (shared dialog chrome)
  draw_dialog_card(app, cr, dlg_x, dlg_y, dlg_w, dlg_h, 12);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, 0), dlg_x, dlg_y, dlg_w, dlg_h);

  int cx = dlg_x + 20;
  int cy = dlg_y + 14;

  // ── Title (with edit icon) ──
  blit_icon(cr, app.edit_svg, cx, cy, 16,
            app.text_secondary_r, app.text_secondary_g, app.text_secondary_b);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, cx + 24, cy + 14);
  char title[64];
  std::snprintf(title, sizeof(title), "Rename %d File%s", n, n == 1 ? "" : "s");
  cairo_show_text(cr, title);

  // ── Mode tabs ──
  int tab_y = cy + 28;
  int tab_h = 26;
  int tab_w = 210;
  cairo_set_font_size(cr, 12);

  auto draw_tab = [&](int tbx, bool active, const char* label, cairo_surface_t* icon) {
    if (active) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.10);
      draw_rounded_rect(cr, tbx, tab_y, tab_w, tab_h, 7);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.9);
      draw_rounded_rect(cr, tbx + 14, tab_y + tab_h - 3, tab_w - 28, 2, 1);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.95);
    } else {
      bool hovered = (tbx == cx && app.batch_rename_hover_mode == 0) ||
                     (tbx == cx + tab_w + 8 && app.batch_rename_hover_mode == 1);
      if (hovered) {
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.12);
        draw_rounded_rect(cr, tbx, tab_y, tab_w, tab_h, 6);
        cairo_fill(cr);
      }
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.5);
    }
    if (icon)
      blit_icon(cr, icon, tbx + 12, tab_y + (tab_h - 14) / 2.0, 14,
                active ? app.accent_r : app.text_secondary_r,
                active ? app.accent_g : app.text_secondary_g,
                active ? app.accent_b : app.text_secondary_b,
                active ? 0.9 : 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_text_extents_t te;
    cairo_text_extents(cr, label, &te);
    cairo_move_to(cr, tbx + (tab_w - static_cast<int>(te.x_advance)) / 2 + 10,
                  tab_y + tab_h / 2 + static_cast<int>(te.height * 0.35));
    cairo_show_text(cr, label);
  };

  draw_tab(cx, app.batch_rename_mode == 0, "Rename using a template", app.edit_svg);
  draw_tab(cx + tab_w + 8, app.batch_rename_mode == 1, "Find and replace text", app.search_svg);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchTab0), cx, tab_y, tab_w, tab_h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchTab1), cx + tab_w + 8, tab_y, tab_w,
                   tab_h);

  int input_y = tab_y + tab_h + 10;
  int field_w = is_template ? 360 : 240;
  int field_h = 30;

  if (is_template) {
    // ── Template mode: single text field + Add button ──
    int tf_x = cx;
    int tf_y = input_y;

    // Input bg
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    draw_rounded_rect(cr, tf_x, tf_y, field_w, field_h, 6);
    cairo_fill(cr);
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchTemplate), tf_x, tf_y, field_w,
                     field_h);

    // Input text
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, tf_x + 8, tf_y + field_h / 2 + 5);
    cairo_show_text(cr, app.batch_rename_template.c_str());

    // Cursor
    cairo_text_extents_t te;
    cairo_text_extents(cr, app.batch_rename_template.substr(0, app.batch_rename_template_cursor).c_str(), &te);
    int cur_x = tf_x + 8 + static_cast<int>(te.x_advance);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.65);
    cairo_rectangle(cr, cur_x, tf_y + 5, 1, field_h - 10);
    cairo_fill(cr);

    // [+ Add] button
    int add_x = tf_x + field_w + 8;
    int add_btn_w = 70;
    bool add_hover = (app.batch_rename_hover_btn == 3);
    cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b,
                          add_hover ? 0.7 : 0.5);
    draw_rounded_rect(cr, add_x, tf_y, add_btn_w, field_h, 6);
    cairo_fill(cr);
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchAdd), add_x, tf_y, add_btn_w,
                     field_h);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.8);
    cairo_set_font_size(cr, 12);
    cairo_text_extents(cr, "+ Add", &te);
    cairo_move_to(cr, add_x + (add_btn_w - static_cast<int>(te.x_advance)) / 2,
                  tf_y + field_h / 2 + static_cast<int>(te.height * 0.35));
    cairo_show_text(cr, "+ Add");

    // Add dropdown list
    if (app.batch_rename_show_add) {
      int dd_x = add_x;
      int dd_y = tf_y + field_h + 2;
      int dd_w = add_btn_w;
      int dd_item_h = 26;
      static const char* add_options[] = {"1, 2, 3...", "01, 02, 03...", "001, 002, 003..."};
      int dd_h = 3 * dd_item_h + 4;

      for (int s = 3; s >= 0; --s) {
        double a = 0.08 * (1.0 - s / 4.0);
        cairo_set_source_rgba(cr, 0, 0, 0, a);
        draw_rounded_rect(cr, dd_x + s * 2, dd_y + s * 2, dd_w, dd_h, 6);
        cairo_fill(cr);
      }
      double tint_r = app.surface_r * 0.65 + app.accent_r * 0.35;
      double tint_g = app.surface_g * 0.65 + app.accent_g * 0.35;
      double tint_b = app.surface_b * 0.65 + app.accent_b * 0.35;
      cairo_set_source_rgba(cr, tint_r, tint_g, tint_b, 1.0);
      draw_rounded_rect(cr, dd_x, dd_y, dd_w, dd_h, 6);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
      cairo_set_line_width(cr, 1);
      draw_rounded_rect(cr, dd_x + 0.5, dd_y + 0.5, dd_w - 1, dd_h - 1, 5.5);
      cairo_stroke(cr);

      cairo_set_font_size(cr, 11);
      for (int i = 0; i < 3; ++i) {
        int iy = dd_y + 2 + i * dd_item_h;
        app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchAddItemBase + i), dd_x, iy, dd_w,
                         dd_item_h);
        if (app.batch_rename_add_hover == i) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.2);
          draw_rounded_rect(cr, dd_x + 4, iy + 1, dd_w - 8, dd_item_h - 2, 4);
          cairo_fill(cr);
        }
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.85);
        cairo_move_to(cr, dd_x + 8, iy + dd_item_h / 2 + 5);
        cairo_show_text(cr, add_options[i]);
      }
    }
  } else {
    // ── Find & Replace mode ──
    int label_w = 100;
    int fld_x = cx + label_w;

    // Existing text label
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
    cairo_move_to(cr, cx, input_y + field_h / 2 + 5);
    cairo_show_text(cr, "Existing text:");

    // Find field bg
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    draw_rounded_rect(cr, fld_x, input_y, field_w, field_h, 6);
    cairo_fill(cr);
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchFind), fld_x, input_y, field_w,
                     field_h);

    // Find field text
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, fld_x + 8, input_y + field_h / 2 + 5);
    cairo_show_text(cr, app.batch_rename_find.c_str());

    // Find field cursor
    if (app.batch_rename_edit_focus == 0) {
      cairo_text_extents_t te;
      cairo_text_extents(cr, app.batch_rename_find.substr(0, app.batch_rename_find_cursor).c_str(), &te);
      int cx2 = fld_x + 8 + static_cast<int>(te.x_advance);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.65);
      cairo_rectangle(cr, cx2, input_y + 5, 1, field_h - 10);
      cairo_fill(cr);
    }

    // Replace with label + field
    int rl_y = input_y + field_h + 6;
    int rl_w = label_w;
    int rf_x = cx + rl_w;

    // Label
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.6);
    cairo_move_to(cr, cx, rl_y + field_h / 2 + 5);
    cairo_show_text(cr, "Replace with:");

    // Replace field bg
    cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
    draw_rounded_rect(cr, rf_x, rl_y, field_w, field_h, 6);
    cairo_fill(cr);
    app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchReplace), rf_x, rl_y, field_w,
                     field_h);

    // Replace field text
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, rf_x + 8, rl_y + field_h / 2 + 5);
    cairo_show_text(cr, app.batch_rename_replace.c_str());

    // Replace field cursor
    if (app.batch_rename_edit_focus == 1) {
      cairo_text_extents_t te;
      cairo_text_extents(cr, app.batch_rename_replace.substr(0, app.batch_rename_replace_cursor).c_str(), &te);
      int cx2 = rf_x + 8 + static_cast<int>(te.x_advance);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.65);
      cairo_rectangle(cr, cx2, rl_y + 5, 1, field_h - 10);
      cairo_fill(cr);
    }
  }

  // ── Preview list ──
  int list_y = input_y + (is_template ? field_h + 10 : field_h * 2 + 16);
  int list_x = cx;
  int list_w = dlg_w - 40;

  // Column headers
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 11);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.45);
  cairo_move_to(cr, list_x + 8, list_y - 4);
  cairo_show_text(cr, "Current Name");
  cairo_text_extents_t te;
  cairo_text_extents(cr, "New Name", &te);
  cairo_move_to(cr, list_x + list_w - 8 - static_cast<int>(te.width), list_y - 4);
  cairo_show_text(cr, "New Name");

  // List bg
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.3);
  draw_rounded_rect(cr, list_x, list_y, list_w, list_h - 4, 6);
  cairo_fill(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  int row_y = list_y + 6;

  for (int i = 0; i < n; ++i) {
    const auto& e = app.batch_rename_entries[i];
    int mid_x = list_x + list_w / 2;

    // Old name (left side)
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.55);
    cairo_move_to(cr, list_x + 8, row_y + 14);
    double max_name_w = list_w / 2 - 28;
    std::string display = hui::design::clip_end(cr, e.old_name, max_name_w);
    cairo_show_text(cr, display.c_str());

    // Arrow in the middle
    blit_icon(cr, app.arrow_right_svg, mid_x - 6, row_y + 8, 12,
              app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.5);

    // New name (right side)
    bool changed = (e.new_name != e.old_name);
    cairo_set_source_rgba(cr, changed ? app.accent_r : app.text_r,
                          changed ? app.accent_g : app.text_g,
                          changed ? app.accent_b : app.text_b,
                          changed ? 0.9 : 0.45);
    std::string ndisplay = hui::design::clip_end(cr, e.new_name, max_name_w);
    cairo_text_extents_t te2;
    cairo_text_extents(cr, ndisplay.c_str(), &te2);
    cairo_move_to(cr, list_x + list_w - 8 - static_cast<int>(te2.width), row_y + 14);
    cairo_show_text(cr, ndisplay.c_str());

    row_y += 26;
  }

  // ── Buttons ──
  int btn_y = dlg_y + dlg_h - 44;
  int btn_w = 90;
  int btn_h = 32;
  int cancel_x = dlg_x + dlg_w - 230;
  int rename_x = dlg_x + dlg_w - 120;
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchCancel), cancel_x, btn_y, btn_w,
                   btn_h);
  app.hit_main.add(hui::Hit::dialog(hui::Hit::kDlgBatch, hui::Hit::kBatchOk), rename_x, btn_y, btn_w, btn_h);

  hui::design::button(cr, app, cancel_x, btn_y, btn_w, btn_h, "Cancel", false,
                 app.batch_rename_hover_btn == 1);

  // Rename button
  int n_changed = 0;
  for (const auto& e : app.batch_rename_entries)
    if (e.new_name != e.old_name) ++n_changed;
  bool can_rename = n_changed > 0;
  if (can_rename) {
    hui::design::button(cr, app, rename_x, btn_y, btn_w, btn_h, "Rename", true,
                   app.batch_rename_hover_btn == 0);
  } else {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.30);
    draw_rounded_rect(cr, rename_x, btn_y, btn_w, btn_h, 9);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13);
    cairo_text_extents_t rte;
    cairo_text_extents(cr, "Rename", &rte);
    cairo_move_to(cr, rename_x + (btn_w - rte.x_advance) / 2,
                  btn_y + btn_h / 2 + rte.height * 0.35);
    cairo_show_text(cr, "Rename");
  }
}

} // namespace eh::file_browser
