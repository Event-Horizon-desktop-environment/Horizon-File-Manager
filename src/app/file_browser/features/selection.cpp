#include "app/file_browser/features/selection.hpp"

#include "../app.hpp"

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cstring>
#include <fnmatch.h>

namespace eh::file_browser {

namespace {

// Dialog geometry — shared between draw and hit-testing.
constexpr int kDlgW = 380;
constexpr int kDlgH = 170;
constexpr int kInputH = 34;
constexpr int kBtnW = 100;
constexpr int kBtnH = 32;

void dialog_geometry(const AppState& app, int& x, int& y) {
  x = (app.width - kDlgW) / 2;
  y = (app.height - kDlgH) / 2;
}

} // namespace

void invert_selection(AppState& app) {
  auto& tab = app.cur_tab();
  auto& sel = tab.multi_selected;
  for (int vi = 0; vi < static_cast<int>(tab.visible_entries.size()); ++vi) {
    auto it = std::find(sel.begin(), sel.end(), vi);
    if (it != sel.end())
      sel.erase(it);
    else
      sel.push_back(vi);
  }
  app.pendingRedraw = true;
}

void open_select_pattern(AppState& app) {
  app.select_pattern_open = true;
  app.select_pattern_buf = "*";
  app.select_pattern_cursor = static_cast<int>(app.select_pattern_buf.size());
  app.select_pattern_hover_btn = -1;
}

void close_select_pattern(AppState& app) {
  app.select_pattern_open = false;
  app.select_pattern_buf.clear();
  app.select_pattern_cursor = 0;
  app.select_pattern_hover_btn = -1;
}

void apply_select_pattern(AppState& app) {
  if (app.select_pattern_buf.empty()) {
    close_select_pattern(app);
    return;
  }
  auto& tab = app.cur_tab();
  for (int vi = 0; vi < static_cast<int>(tab.visible_entries.size()); ++vi) {
    int ri = tab.visible_entries[vi];
    if (ri < 0 || ri >= static_cast<int>(tab.entries.size())) continue;
    const std::string& name = tab.entries[ri].name;
    if (fnmatch(app.select_pattern_buf.c_str(), name.c_str(), 0) == 0) {
      if (std::find(tab.multi_selected.begin(), tab.multi_selected.end(), vi) ==
          tab.multi_selected.end())
        tab.multi_selected.push_back(vi);
    }
  }
  close_select_pattern(app);
  app.pendingRedraw = true;
}

void draw_select_pattern_dialog(AppState& app, cairo_t* cr) {
  int dlg_x, dlg_y;
  dialog_geometry(app, dlg_x, dlg_y);
  double sa = app.surface_opacity_pct / 100.0;

  // Dimmed backdrop
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, app.width, app.height);
  cairo_fill(cr);

  // Card
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, sa);
  draw_rounded_rect(cr, dlg_x, dlg_y, kDlgW, kDlgH, 10);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, dlg_x + 0.5, dlg_y + 0.5, kDlgW - 1, kDlgH - 1, 9.5);
  cairo_stroke(cr);

  // Title
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 15);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, dlg_x + 20, dlg_y + 30);
  cairo_show_text(cr, "Select Items Matching");

  // Example hint
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 0.9);
  cairo_move_to(cr, dlg_x + 20, dlg_y + 48);
  cairo_show_text(cr, "Examples: *.png, file??.txt, pict*.???");

  // Input field
  int input_x = dlg_x + 20;
  int input_y = dlg_y + 60;
  int input_w = kDlgW - 40;
  cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, 0.5);
  draw_rounded_rect(cr, input_x, input_y, input_w, kInputH, 6);
  cairo_fill(cr);

  cairo_set_font_size(cr, 14);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, input_x + 10, input_y + kInputH / 2 + 4);
  cairo_show_text(cr, app.select_pattern_buf.c_str());

  // Caret
  {
    std::string before = app.select_pattern_buf.substr(0, static_cast<size_t>(app.select_pattern_cursor));
    cairo_text_extents_t te;
    cairo_text_extents(cr, before.c_str(), &te);
    double cx = input_x + 10 + te.width;
    cairo_set_line_width(cr, 1.4);
    cairo_move_to(cr, cx, input_y + 7);
    cairo_line_to(cr, cx, input_y + kInputH - 7);
    cairo_stroke(cr);
  }

  // Buttons: Cancel (left), Select (right)
  int btn_y = dlg_y + kDlgH - 50;
  int cancel_x = dlg_x + kDlgW - 220;
  int ok_x = dlg_x + kDlgW - 110;

  auto draw_btn = [&](int bx, const char* label, bool accent, bool hover) {
    if (accent)
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                            hover ? 1.0 : 0.85);
    else
      cairo_set_source_rgba(cr, app.bg_r, app.bg_g, app.bg_b, hover ? 0.9 : 0.6);
    draw_rounded_rect(cr, bx, btn_y, kBtnW, kBtnH, 6);
    cairo_fill(cr);
    cairo_text_extents_t te;
    cairo_text_extents(cr, label, &te);
    cairo_set_source_rgba(cr, accent ? 1.0 : app.text_r,
                          accent ? 1.0 : app.text_g,
                          accent ? 1.0 : app.text_b, 1.0);
    cairo_move_to(cr, bx + (kBtnW - te.width) / 2, btn_y + kBtnH / 2 + 4);
    cairo_show_text(cr, label);
  };

  draw_btn(cancel_x, "Cancel", false, app.select_pattern_hover_btn == 1);
  draw_btn(ok_x, "Select", true, app.select_pattern_hover_btn == 2);
}

bool handle_select_pattern_key(AppState& app, uint32_t keysym,
                               const char* utf8, int utf8_len) {
  if (!app.select_pattern_open) return false;

  if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
    apply_select_pattern(app);
    return true;
  }
  if (keysym == XKB_KEY_Escape) {
    close_select_pattern(app);
    app.pendingRedraw = true;
    return true;
  }
  if (keysym == XKB_KEY_BackSpace) {
    if (app.select_pattern_cursor > 0) {
      // Erase one UTF-8 code point before the cursor
      int pos = app.select_pattern_cursor - 1;
      while (pos > 0 &&
             (static_cast<unsigned char>(app.select_pattern_buf[pos]) & 0xC0) == 0x80)
        --pos;
      app.select_pattern_buf.erase(static_cast<size_t>(pos),
                                   static_cast<size_t>(app.select_pattern_cursor - pos));
      app.select_pattern_cursor = pos;
    }
    app.pendingRedraw = true;
    return true;
  }
  if (keysym == XKB_KEY_Left) {
    if (app.select_pattern_cursor > 0) {
      int pos = app.select_pattern_cursor - 1;
      while (pos > 0 &&
             (static_cast<unsigned char>(app.select_pattern_buf[pos]) & 0xC0) == 0x80)
        --pos;
      app.select_pattern_cursor = pos;
    }
    app.pendingRedraw = true;
    return true;
  }
  if (keysym == XKB_KEY_Right) {
    if (app.select_pattern_cursor < static_cast<int>(app.select_pattern_buf.size())) {
      ++app.select_pattern_cursor;
      while (app.select_pattern_cursor < static_cast<int>(app.select_pattern_buf.size()) &&
             (static_cast<unsigned char>(app.select_pattern_buf[app.select_pattern_cursor]) &
              0xC0) == 0x80)
        ++app.select_pattern_cursor;
    }
    app.pendingRedraw = true;
    return true;
  }
  if (keysym == XKB_KEY_Home) {
    app.select_pattern_cursor = 0;
    app.pendingRedraw = true;
    return true;
  }
  if (keysym == XKB_KEY_End) {
    app.select_pattern_cursor = static_cast<int>(app.select_pattern_buf.size());
    app.pendingRedraw = true;
    return true;
  }
  if (utf8 && utf8_len > 0 && static_cast<unsigned char>(utf8[0]) >= 32) {
    app.select_pattern_buf.insert(
        static_cast<size_t>(app.select_pattern_cursor), utf8,
        static_cast<size_t>(utf8_len));
    app.select_pattern_cursor += utf8_len;
    app.pendingRedraw = true;
    return true;
  }
  return true; // swallow everything else while modal
}

bool handle_select_pattern_click(AppState& app, int button, int x, int y) {
  if (!app.select_pattern_open) return false;

  int dlg_x, dlg_y;
  dialog_geometry(app, dlg_x, dlg_y);
  int btn_y = dlg_y + kDlgH - 50;
  int cancel_x = dlg_x + kDlgW - 220;
  int ok_x = dlg_x + kDlgW - 110;

  if (button == 0x110) { // left press
    if (x >= ok_x && x < ok_x + kBtnW && y >= btn_y && y < btn_y + kBtnH) {
      apply_select_pattern(app);
      return true;
    }
    if (x >= cancel_x && x < cancel_x + kBtnW && y >= btn_y && y < btn_y + kBtnH) {
      close_select_pattern(app);
      app.pendingRedraw = true;
      return true;
    }
    // Any click inside the card lands on the input — focus stays; outside cancels
    if (x < dlg_x || x >= dlg_x + kDlgW || y < dlg_y || y >= dlg_y + kDlgH) {
      close_select_pattern(app);
      app.pendingRedraw = true;
      return true;
    }
    return true; // absorbed by the modal
  }
  return false;
}

} // namespace eh::file_browser
