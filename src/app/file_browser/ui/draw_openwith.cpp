#include "../app.hpp"
#include "../trace.hpp"
#include "../features/sidebar/sidebar.hpp"
#include "../features/view_zoom/view_zoom.hpp"
#include "app/file_browser/features/thumbnails/thumb_pool.hpp"
#include "app/file_browser/features/dir_stats/dir_stats.hpp"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "draw_helpers.hpp"
#include "ui/design.hpp"
#include "draw_file_icons.hpp"
#include "draw_thumbnails.hpp"
#include "layout.hpp"

#include "platform/common/icon_cache/icon_cache.hpp"
#include "app/file_browser/features/preview/svg_preview.hpp"
#include "app/file_browser/features/preview/video_preview.hpp"
#include "app/file_browser/features/preview/pdf_preview.hpp"
#include "app/file_browser/features/preview/epub_preview.hpp"
#include "app/file_browser/features/preview/image_preview.hpp"
#include "app/file_browser/features/thumbnails/thumbnail_cache.hpp"
#include "app/file_browser/ui/pixblit.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {


namespace {

void draw_pill_button(cairo_t* cr, const AppState& app, double x, double y,
                      double w, double h, const char* label, bool primary,
                      bool hovered) {
  const double r = h / 2;
  if (primary) {
    const double bright = hovered ? 1.0 : 0.92;
    cairo_set_source_rgba(cr, app.accent_r * bright, app.accent_g * bright,
                          app.accent_b * bright, 1.0);
  } else {
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                          hovered ? 0.12 : 0.055);
  }
  draw_rounded_rect(cr, x, y, w, h, r);
  cairo_fill(cr);
  if (!primary) {
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.30);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r - 0.5);
    cairo_stroke(cr);
  }

  double lr = app.text_r, lg = app.text_g, lb = app.text_b;
  if (primary) {
    const double lum =
        app.accent_r * 0.299 + app.accent_g * 0.587 + app.accent_b * 0.114;
    if (lum > 0.62) {
      lr = app.bg_r * 0.45;
      lg = app.bg_g * 0.45;
      lb = app.bg_b * 0.45;
    } else {
      lr = lg = lb = 1.0;
    }
  }
  cairo_set_source_rgba(cr, lr, lg, lb, primary ? 0.98 : 0.92);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         primary ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 14);
  cairo_text_extents_t te;
  cairo_text_extents(cr, label, &te);
  cairo_move_to(cr, x + (w - te.x_advance) / 2, y + h / 2 + te.height * 0.35);
  cairo_show_text(cr, label);
}

}  // namespace

struct OpenWithLayout {
  int total_entries = 0, rec_count = 0, other_count = 0;
  int card_w = 440, pad_x = 22, pad_top = 22, file_w = 46, file_h = 54;
  int header_h = 54, search_h = 36, entry_h = 48, section_h = 28;
  int list_pad_bottom = 10, max_list_h = 360, footer_h = 70, btn_h = 36;
  int content_h = 0, list_h = 0, max_scroll = 0;
  int search_y_rel = 0, list_y_rel = 0, footer_y_rel = 0;
  int card_h = 0, cx = 0, cy = 0;
};

static OpenWithLayout compute_open_with_layout(AppState& app) {
  OpenWithLayout L;
  L.total_entries = static_cast<int>(app.open_with_visible.size());
  L.rec_count = app.open_with_visible_exact_count;
  L.other_count = L.total_entries - L.rec_count;

  L.content_h = L.total_entries * L.entry_h;
  if (L.rec_count > 0) L.content_h += L.section_h;
  if (L.other_count > 0) L.content_h += L.section_h;
  L.list_h = L.total_entries == 0 ? 120 : std::min(L.content_h, L.max_list_h);
  L.max_scroll = std::max(0, L.content_h - L.list_h);
  app.open_with_scroll_smooth_target =
      std::clamp(app.open_with_scroll_smooth_target, 0.0,
                 static_cast<double>(L.max_scroll));
  app.open_with_scroll = std::clamp(app.open_with_scroll, 0, L.max_scroll);

  L.search_y_rel = L.pad_top + L.header_h + 14;
  L.list_y_rel = L.search_y_rel + L.search_h + 8;
  L.footer_y_rel = L.list_y_rel + L.list_h + L.list_pad_bottom;
  L.card_h = L.footer_y_rel + L.footer_h;
  L.cx = (app.width - L.card_w) / 2;
  L.cy = (app.height - L.card_h) / 2;

  app.open_with_w = static_cast<double>(L.card_w);
  app.open_with_h = static_cast<double>(L.card_h);
  app.open_with_x = static_cast<double>(L.cx);
  app.open_with_y = static_cast<double>(L.cy);
  return L;
}

void open_with_popup_rect(AppState& app, int& x, int& y, int& w, int& h) {
  const OpenWithLayout L = compute_open_with_layout(app);
  x = L.cx;
  y = L.cy;
  w = L.card_w;
  h = L.card_h;
}

void draw_open_with_backdrop(AppState& app, cairo_t* cr) {
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_rectangle(cr, 0, 0, app.width, app.height);
  cairo_fill(cr);
}

void draw_open_with(AppState& app, cairo_t* cr) {
  const OpenWithLayout L = compute_open_with_layout(app);
  const int total_entries = L.total_entries;
  const int rec_count = L.rec_count;
  const int other_count = L.other_count;
  const int card_w = L.card_w;
  const int pad_x = L.pad_x;
  const int pad_top = L.pad_top;
  const int file_w = L.file_w, file_h = L.file_h;
  const int header_h = L.header_h;
  const int search_h = L.search_h;
  const int entry_h = L.entry_h;
  const int section_h = L.section_h;
  const int list_pad_bottom = L.list_pad_bottom;
  const int max_list_h = L.max_list_h;
  const int footer_h = L.footer_h;
  const int btn_h = L.btn_h;
  const int content_h = L.content_h;
  const int list_h = L.list_h;
  const int max_scroll = L.max_scroll;
  const int search_y_rel = L.search_y_rel;
  const int list_y_rel = L.list_y_rel;
  const int footer_y_rel = L.footer_y_rel;
  const int card_h = L.card_h;
  const int cx = L.cx;
  const int cy = L.cy;

  draw_dialog_card(app, cr, cx, cy, card_w, card_h, 30,
                   app.open_with_opacity_pct / 100.0);

  // ── Header ────────────────────────────────────────────────────────────
  const int file_x = cx + pad_x;
  const int file_y = cy + pad_top;
  std::string ext = fs::path(app.open_with_file_path).extension().string();
  if (!ext.empty() && ext[0] == '.') ext.erase(ext.begin());
  for (char& c : ext)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (ext.size() > 4) ext.resize(4);

  // File tile tinted by the file's broad type, matching the mock's warm
  // archive card while staying inside the app palette.
  double tr = app.accent_r, tg = app.accent_g, tb = app.accent_b;
  {
    std::string low = fs::path(app.open_with_file_path).extension().string();
    for (char& c : low)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == ".zip" || low == ".tar" || low == ".gz" || low == ".xz" ||
        low == ".7z" || low == ".rar" || low == ".bz2") {
      tr = 0.91; tg = 0.77; tb = 0.42;
    } else if (low == ".png" || low == ".jpg" || low == ".jpeg" ||
               low == ".webp" || low == ".gif" || low == ".svg") {
      tr = 0.61; tg = 0.78; tb = 0.42;
    } else if (low == ".mp4" || low == ".mkv" || low == ".webm" ||
               low == ".avi" || low == ".mov") {
      tr = 0.72; tg = 0.52; tb = 0.88;
    } else if (low == ".mp3" || low == ".flac" || low == ".ogg" ||
               low == ".wav" || low == ".m4a") {
      tr = 0.41; tg = 0.63; tb = 0.93;
    } else if (low == ".pdf" || low == ".epub" || low == ".doc" ||
               low == ".docx" || low == ".odt") {
      tr = 0.42; tg = 0.59; tb = 0.90;
    }
  }
  {
    cairo_pattern_t* grad = cairo_pattern_create_linear(
        file_x, file_y, file_x + file_w, file_y + file_h);
    cairo_pattern_add_color_stop_rgb(grad, 0.0, std::min(1.0, tr * 1.12 + 0.06),
                                     std::min(1.0, tg * 1.12 + 0.06),
                                     std::min(1.0, tb * 1.12 + 0.06));
    cairo_pattern_add_color_stop_rgb(grad, 1.0, tr * 0.74, tg * 0.74, tb * 0.74);
    cairo_set_source(cr, grad);
    draw_rounded_rect(cr, file_x, file_y, file_w, file_h, 10);
    cairo_fill(cr);
    cairo_pattern_destroy(grad);
  }
  // Page fold + a few document rules.
  cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
  cairo_move_to(cr, file_x + file_w - 13, file_y);
  cairo_line_to(cr, file_x + file_w, file_y);
  cairo_line_to(cr, file_x + file_w, file_y + 13);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
  for (int i = 0; i < 3; ++i) {
    cairo_rectangle(cr, file_x + 10, file_y + 9 + i * 9, file_w - 25 - i * 4, 2);
    cairo_fill(cr);
  }
  if (!ext.empty()) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 9);
    cairo_text_extents_t te;
    cairo_text_extents(cr, ext.c_str(), &te);
    cairo_set_source_rgba(cr, 0.20, 0.16, 0.06, 0.82);
    cairo_move_to(cr, file_x + (file_w - te.x_advance) / 2,
                  file_y + file_h - 8);
    cairo_show_text(cr, ext.c_str());
  }

  const int title_x = file_x + file_w + 14;
  const int close_sz = 28;
  const int close_x = cx + card_w - pad_x - close_sz;
  const int close_y = cy + pad_top;
  app.open_with_hit_close[0] = close_x;
  app.open_with_hit_close[1] = close_y;
  app.open_with_hit_close[2] = close_sz;
  app.open_with_hit_close[3] = close_sz;

  const int title_max = close_x - title_x - 10;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 17);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
  cairo_move_to(cr, title_x, cy + pad_top + 18);
  cairo_show_text(cr, "Open With");

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12.5);
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 0.9);
  std::string fname = fs::path(app.open_with_file_path).filename().string();
  cairo_move_to(cr, title_x, cy + pad_top + 37);
  cairo_show_text(cr, hui::design::clip_end(cr, fname, std::max(20, title_max)).c_str());

  {
    const bool hov = (app.open_with_hover == -2);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                          hov ? 0.12 : 0.06);
    cairo_arc(cr, close_x + close_sz / 2.0, close_y + close_sz / 2.0,
              close_sz / 2.0, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b,
                          hov ? 0.92 : 0.52);
    cairo_set_line_width(cr, 1.7);
    cairo_move_to(cr, close_x + 9, close_y + 9);
    cairo_line_to(cr, close_x + 19, close_y + 19);
    cairo_move_to(cr, close_x + 19, close_y + 9);
    cairo_line_to(cr, close_x + 9, close_y + 19);
    cairo_stroke(cr);
  }

  // ── Search field ──────────────────────────────────────────────────────
  const int search_x = cx + pad_x;
  const int search_y = cy + search_y_rel;
  const int search_w = card_w - pad_x * 2;
  app.open_with_hit_search[0] = search_x;
  app.open_with_hit_search[1] = search_y;
  app.open_with_hit_search[2] = search_w;
  app.open_with_hit_search[3] = search_h;

  cairo_set_source_rgba(cr, app.bg_r * 0.92, app.bg_g * 0.92, app.bg_b * 0.92, 0.55);
  draw_rounded_rect(cr, search_x, search_y, search_w, search_h, 12);
  cairo_fill(cr);

  {
    const double mag_x = search_x + 15, mag_y = search_y + search_h / 2.0;
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.72);
    cairo_set_line_width(cr, 1.7);
    cairo_arc(cr, mag_x, mag_y - 1, 5, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, mag_x + 4, mag_y + 3);
    cairo_line_to(cr, mag_x + 8, mag_y + 7);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    const int text_x = search_x + 34;
    const int text_max = search_w - 46;
    if (app.open_with_query.empty()) {
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 0.62);
      cairo_move_to(cr, text_x, search_y + search_h / 2 + 5);
      cairo_show_text(cr, "Search applications");
    } else {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_move_to(cr, text_x, search_y + search_h / 2 + 5);
      cairo_show_text(cr, hui::design::clip_end(cr, app.open_with_query,
                                                text_max).c_str());
    }
  }

  // ── Application list ──────────────────────────────────────────────────
  const int list_x = cx + 14;
  const int list_y = cy + list_y_rel;
  const int list_w = card_w - 28;
  const int scrollbar_w = 5;
  const int scroll_px = app.open_with_scroll;

  std::string default_id = app.open_with_default_id;
  if (auto slash = default_id.rfind('/'); slash != std::string::npos)
    default_id = default_id.substr(slash + 1);
  if (default_id.size() > 8 &&
      default_id.compare(default_id.size() - 8, 8, ".desktop") == 0)
    default_id.resize(default_id.size() - 8);
  auto is_default = [&](const AppState::OpenWithEntry& e) {
    return !default_id.empty() &&
           (e.desktop_id == default_id ||
            e.desktop_id == default_id + ".desktop");
  };

  cairo_save(cr);
  cairo_rectangle(cr, list_x, list_y, list_w, list_h);
  cairo_clip(cr);

  if (total_entries == 0) {
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.72);
    const char* msg = "No applications found";
    cairo_text_extents_t te;
    cairo_text_extents(cr, msg, &te);
    cairo_move_to(cr, list_x + (list_w - te.x_advance) / 2,
                  list_y + list_h / 2 + te.height * 0.35);
    cairo_show_text(cr, msg);
  }

  int cy_off = 0;
  int visible_pos = 0;
  for (int ai : app.open_with_visible) {
    if (visible_pos == 0 && rec_count > 0) {
      if (cy_off + section_h > scroll_px && cy_off < scroll_px + list_h) {
        int sy = list_y + cy_off - scroll_px;
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 0.70);
        cairo_move_to(cr, list_x + 10, sy + 18);
        cairo_show_text(cr, "RECOMMENDED");
      }
      cy_off += section_h;
    }
    if (visible_pos == rec_count && other_count > 0) {
      if (cy_off + section_h > scroll_px && cy_off < scroll_px + list_h) {
        int sy = list_y + cy_off - scroll_px;
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11);
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 0.70);
        cairo_move_to(cr, list_x + 10, sy + 18);
        cairo_show_text(cr, "ALL APPLICATIONS");
      }
      cy_off += section_h;
    }

    if (cy_off + entry_h > scroll_px && cy_off < scroll_px + list_h) {
      const int ey = list_y + cy_off - scroll_px;
      const auto& entry = app.open_with_apps[ai];
      const bool selected = (ai == app.open_with_selected);
      const bool hov = (ai == app.open_with_hover);

      app.hit_main.add(
          hui::Hit::dialog(hui::Hit::kDlgOpenWith, hui::Hit::kOpenRowBase + ai),
          list_x + 2, ey, list_w - 4, entry_h);

      if (selected || hov) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b,
                              selected ? 0.25 : 0.10);
        draw_rounded_rect(cr, list_x + 2, ey + 2, list_w - 4, entry_h - 4, 14);
        cairo_fill(cr);
        if (selected) {
          cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.22);
          cairo_set_line_width(cr, 1);
          draw_rounded_rect(cr, list_x + 2.5, ey + 2.5, list_w - 5, entry_h - 5, 13.5);
          cairo_stroke(cr);
        }
      }

      const int icon_size = 34;
      const int icon_x = list_x + 10;
      const int icon_y = ey + (entry_h - icon_size) / 2;
      // Prefer the desktop entry's own Icon= value: Steam games use
      // steam_icon_<appid>, many Flatpak apps use reverse-DNS names that do
      // not match the desktop file stem, and some entries carry an absolute
      // path. Fall back to the desktop ID when Icon= is absent or unresolved.
      const eh::icons::IconEntry* icon_entry = nullptr;
      if (!entry.icon.empty())
        icon_entry = app.icons.app_icon(entry.icon, 48);
      if ((!icon_entry || !icon_entry->surface) && !entry.desktop_id.empty())
        icon_entry = app.icons.app_icon(entry.desktop_id, 48);
      if (icon_entry && icon_entry->surface) {
        double iw = static_cast<double>(icon_entry->width);
        double ih = static_cast<double>(icon_entry->height);
        double scale = icon_size / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, icon_x, icon_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, icon_entry->surface,
                                 ((icon_size / scale) - iw) * 0.5,
                                 ((icon_size / scale) - ih) * 0.5);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        cairo_set_source_rgba(cr, app.accent_r * 0.82, app.accent_g * 0.82,
                              app.accent_b * 0.82, 1.0);
        draw_rounded_rect(cr, icon_x, icon_y, icon_size, icon_size, 9);
        cairo_fill(cr);
        char letter[2] = {entry.name.empty() ? '?' : entry.name[0], '\0'};
        cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 14);
        cairo_text_extents_t te;
        cairo_text_extents(cr, letter, &te);
        cairo_move_to(cr, icon_x + (icon_size - te.width) * 0.5 - te.x_bearing,
                      icon_y + (icon_size + te.height) * 0.5 - te.y_bearing);
        cairo_show_text(cr, letter);
      }

      const int check_sz = 18;
      const int check_x = list_x + list_w - 12 - check_sz;
      const int check_y = ey + (entry_h - check_sz) / 2;
      int name_right = check_x - 10;

      if (is_default(entry)) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        cairo_text_extents_t te;
        cairo_text_extents(cr, "Default", &te);
        name_right -= static_cast<int>(te.x_advance) + 12;
        cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                              app.text_secondary_b, 0.72);
        cairo_move_to(cr, name_right + 12, ey + entry_h / 2 + 4);
        cairo_show_text(cr, "Default");
      }

      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             selected ? CAIRO_FONT_WEIGHT_BOLD
                                      : CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 14.5);
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      const int name_x = icon_x + icon_size + 12;
      cairo_move_to(cr, name_x, ey + entry_h / 2 + 5);
      cairo_show_text(cr, hui::design::clip_end(
                              cr, entry.name,
                              std::max(20, name_right - name_x)).c_str());

      if (selected) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
        cairo_arc(cr, check_x + check_sz / 2.0, check_y + check_sz / 2.0,
                  check_sz / 2.0, 0, 2 * M_PI);
        cairo_fill(cr);
        const double lum = app.accent_r * 0.299 + app.accent_g * 0.587 +
                           app.accent_b * 0.114;
        if (lum > 0.62)
          cairo_set_source_rgba(cr, app.bg_r * 0.45, app.bg_g * 0.45,
                                app.bg_b * 0.45, 1.0);
        else
          cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_set_line_width(cr, 1.8);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, check_x + 5, check_y + 9);
        cairo_line_to(cr, check_x + 8, check_y + 12);
        cairo_line_to(cr, check_x + 13, check_y + 6);
        cairo_stroke(cr);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
      }
    }

    cy_off += entry_h;
    ++visible_pos;
  }

  cairo_restore(cr);

  if (content_h > list_h) {
    const int sb_h = std::max(scrollbar_w * 2, list_h * list_h / content_h);
    const int sb_max = list_h - sb_h;
    const double frac = max_scroll > 0
        ? static_cast<double>(scroll_px) / static_cast<double>(max_scroll)
        : 0.0;
    const int sb_y = list_y + static_cast<int>(frac * sb_max);
    const int sx = list_x + list_w - scrollbar_w - 1;
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.40);
    draw_rounded_rect(cr, sx, sb_y, scrollbar_w, sb_h, scrollbar_w / 2);
    cairo_fill(cr);
  }

  // ── Footer ────────────────────────────────────────────────────────────
  const int sep2_y = cy + footer_y_rel;
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
  cairo_rectangle(cr, cx, sep2_y, card_w, 1);
  cairo_fill(cr);

  const int btn_y = sep2_y + (footer_h - btn_h) / 2;
  const int btn_gap = 10;
  const int btn_w = 84;
  const int btn_right = cx + card_w - pad_x;

  app.open_with_hit_cancel[0] = btn_right - btn_w * 2 - btn_gap;
  app.open_with_hit_cancel[1] = btn_y;
  app.open_with_hit_cancel[2] = btn_w;
  app.open_with_hit_cancel[3] = btn_h;

  app.open_with_hit_open[0] = btn_right - btn_w;
  app.open_with_hit_open[1] = btn_y;
  app.open_with_hit_open[2] = btn_w;
  app.open_with_hit_open[3] = btn_h;

  draw_pill_button(cr, app, app.open_with_hit_cancel[0], btn_y, btn_w, btn_h,
                   "Cancel", false, app.open_with_hover == -3);

  {
    const int toggle_x = cx + pad_x;
    const int toggle_h = 36;
    const int toggle_y = sep2_y + (footer_h - toggle_h) / 2;
    const bool hov = (app.open_with_hover == -5);

    app.open_with_hit_default[0] = toggle_x;
    app.open_with_hit_default[1] = toggle_y;
    app.open_with_hit_default[2] = 214;
    app.open_with_hit_default[3] = toggle_h;

    hui::design::draw_switch(cr, app, toggle_x, toggle_y + (toggle_h - 22) / 2,
                             app.open_with_set_default);

    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, hov ? 0.95 : 0.78);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_move_to(cr, toggle_x + 52, toggle_y + toggle_h / 2 + 5);
    cairo_show_text(cr, "Always open with this app");
  }

  draw_pill_button(cr, app, app.open_with_hit_open[0], btn_y, btn_w, btn_h,
                   "Open", true, app.open_with_hover == -4);
}

} // namespace eh::file_browser
