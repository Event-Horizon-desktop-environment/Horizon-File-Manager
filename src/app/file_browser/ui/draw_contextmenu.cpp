#include "../app.hpp"
#include "../trace.hpp"
#include "../features/sidebar/sidebar.hpp"
#include "../features/tags/tags.hpp"
#include "../features/view_zoom/view_zoom.hpp"
#include "app/file_browser/features/thumbnails/thumb_pool.hpp"
#include "app/file_browser/features/dir_stats/dir_stats.hpp"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
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

constexpr int kMenuRowH = 36;
constexpr int kMenuSepH = 13;
constexpr int kMenuPad = 8;
constexpr int kMenuRadius = 22;
constexpr int kMenuRowRadius = 12;
constexpr int kMenuItemPadX = 12;

int menu_items_height(const std::vector<AppState::ContextMenuItem>& items) {
  int h = 0;
  for (const auto& item : items) {
    h += (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty())
             ? kMenuSepH
             : kMenuRowH;
  }
  return h;
}

struct ContextMenuGeometry {
  int cm_x = 0, cm_y = 0, cm_w = 248, cm_h = 0;
};

ContextMenuGeometry context_menu_geometry(AppState& app) {
  ContextMenuGeometry g;
  g.cm_x = app.context_menu_x;
  g.cm_y = app.context_menu_y;
  g.cm_h = menu_items_height(app.context_menu_items) + kMenuPad * 2;
  clamp_popup_rect(app, g.cm_x, g.cm_y, g.cm_w, g.cm_h);
  return g;
}

struct SubmenuGeometry {
  int sub_x = 0, sub_y = 0, sm_w = 216, sm_h = 0;
};

SubmenuGeometry submenu_geometry(AppState& app, int hover) {
  SubmenuGeometry g;
  const auto& items = app.context_menu_items;
  const ContextMenuGeometry m = context_menu_geometry(app);
  g.sm_h = menu_items_height(items[static_cast<size_t>(hover)].sub_items) +
           kMenuPad * 2;
  g.sub_x = m.cm_x + m.cm_w + 4;
  if (g.sub_x + g.sm_w > app.width - 8) g.sub_x = m.cm_x - g.sm_w - 4;
  g.sub_y = m.cm_y + kMenuPad;
  for (int j = 0; j < hover; ++j) {
    g.sub_y += (items[static_cast<size_t>(j)].action == AppState::ContextMenuAction::Separator &&
                items[static_cast<size_t>(j)].sub_items.empty())
                   ? kMenuSepH
                   : kMenuRowH;
  }
  clamp_popup_rect(app, g.sub_x, g.sub_y, g.sm_w, g.sm_h);
  return g;
}

// Tahoe glass card: translucent wallpaper-tinted fill, hairline stroke and a
// top inset highlight. No drop shadow: the compositor blurs the whole layer,
// so any painted shadow becomes a visible halo.
void draw_menu_card(AppState& app, cairo_t* cr, double x, double y, double w,
                    double h, double alpha) {
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 0.80 * alpha);
  draw_rounded_rect(cr, x, y, w, h, kMenuRadius);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1, 1, 1, 0.14 * alpha + 0.05);
  cairo_set_line_width(cr, 1.0);
  draw_rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, kMenuRadius - 0.5);
  cairo_stroke(cr);

  cairo_save(cr);
  draw_rounded_rect(cr, x, y, w, h, kMenuRadius);
  cairo_clip(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.16 * alpha);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, x + kMenuRadius, y + 1.0);
  cairo_line_to(cr, x + w - kMenuRadius, y + 1.0);
  cairo_stroke(cr);
  cairo_restore(cr);
}

void draw_menu_row_hover(cairo_t* cr, const AppState& app, double x, double y,
                         double w, double h, bool danger) {
  if (danger) {
    cairo_set_source_rgba(cr, 1.0, 0.39, 0.36, 0.20);
    draw_rounded_rect(cr, x, y, w, h, kMenuRowRadius);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1.0, 0.5, 0.46, 0.20);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1,
                      kMenuRowRadius - 0.5);
    cairo_stroke(cr);
    return;
  }
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.24);
  draw_rounded_rect(cr, x, y, w, h, kMenuRowRadius);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1,
                    kMenuRowRadius - 0.5);
  cairo_stroke(cr);
}

void draw_menu_chevron(cairo_t* cr, const AppState& app, double cx,
                       double cy) {
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 0.85);
  cairo_set_line_width(cr, 1.8);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_move_to(cr, cx - 2.0, cy - 4.0);
  cairo_line_to(cr, cx + 2.0, cy);
  cairo_line_to(cr, cx - 2.0, cy + 4.0);
  cairo_stroke(cr);
}

const char* menu_shortcut(AppState::ContextMenuAction action) {
  switch (action) {
    case AppState::ContextMenuAction::Copy: return "Ctrl+C";
    case AppState::ContextMenuAction::Paste: return "Ctrl+V";
    case AppState::ContextMenuAction::Rename: return "F2";
    case AppState::ContextMenuAction::HideFile: return "Ctrl+H";
    case AppState::ContextMenuAction::MoveToTrash: return "Del";
    case AppState::ContextMenuAction::PermanentDelete: return "Shift+Del";
    default: return nullptr;
  }
}

void draw_menu_rows(AppState& app, cairo_t* cr,
                    const std::vector<AppState::ContextMenuItem>& items,
                    int card_x, int card_y, int card_w, int hover,
                    bool submenu) {
  const int row_x = card_x + kMenuPad;
  const int row_w = card_w - kMenuPad * 2;
  int ry = card_y + kMenuPad;

  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    const auto& item = items[i];
    const bool separator =
        item.action == AppState::ContextMenuAction::Separator &&
        item.sub_items.empty();

    if (separator) {
      app.hit_main.add(
          hui::Hit::menu(submenu ? hui::Hit::kMenuCtxSub : hui::Hit::kMenuCtx, i),
          row_x, ry, row_w, kMenuSepH);
      hui::design::hairline(cr, app, row_x, ry + kMenuSepH / 2, row_w, 8);
      ry += kMenuSepH;
      continue;
    }

    const bool danger =
        item.action == AppState::ContextMenuAction::MoveToTrash ||
        item.action == AppState::ContextMenuAction::PermanentDelete;
    const bool has_sub = !item.sub_items.empty();
    const bool hovered = (i == hover);
    const char* shortcut = has_sub ? nullptr : menu_shortcut(item.action);

    app.hit_main.add(
        hui::Hit::menu(submenu ? hui::Hit::kMenuCtxSub : hui::Hit::kMenuCtx, i),
        row_x, ry, row_w, kMenuRowH);

    if (hovered)
      draw_menu_row_hover(cr, app, row_x, ry, row_w, kMenuRowH, danger);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    if (danger)
      cairo_set_source_rgba(cr, 0.96, 0.32, 0.32, 0.95);
    else
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.98);

    double right_reserve = 14;
    if (has_sub) right_reserve = 34;
    if (shortcut) {
      cairo_text_extents_t ke;
      cairo_text_extents(cr, shortcut, &ke);
      right_reserve = ke.x_advance + 18;
    }

    const double avail = row_w - kMenuItemPadX - right_reserve;
    std::string shown = hui::design::clip_end(cr, item.label, avail);
    cairo_text_extents_t te;
    cairo_text_extents(cr, shown.c_str(), &te);
    const double tx = row_x + kMenuItemPadX;
    const double ty = ry + (kMenuRowH - te.height) / 2.0 - te.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, shown.c_str());

    if (has_sub) {
      draw_menu_chevron(cr, app, row_x + row_w - 16, ry + kMenuRowH / 2.0);
    } else if (shortcut) {
      cairo_set_font_size(cr, 11.5);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 0.72);
      cairo_text_extents_t ke;
      cairo_text_extents(cr, shortcut, &ke);
      cairo_move_to(cr, row_x + row_w - 12 - ke.x_advance,
                    ry + (kMenuRowH - ke.height) / 2.0 - ke.y_bearing);
      cairo_show_text(cr, shortcut);
    }

    // Tag toggles: filled when the target carries the tag, outline otherwise.
    if (item.action == AppState::ContextMenuAction::TagToggle) {
      double rr = 0.55, gg = 0.55, bb = 0.55;
      for (auto& t : tag_registry(app.tag_colors)) {
        if (t.name == item.data) {
          rr = t.r; gg = t.g; bb = t.b;
          break;
        }
      }
      bool on = false;
      if (app.context_menu_file_idx >= 0 &&
          app.context_menu_file_idx <
              static_cast<int>(app.cur_tab().visible_entries.size())) {
        int ri = app.cur_tab().visible_entries[static_cast<size_t>(
            app.context_menu_file_idx)];
        if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size()))
          on = has_tag(app.cur_tab().entries[ri].tags_csv, item.data);
      }
      const double dx = row_x + row_w - 20, dy = ry + kMenuRowH / 2.0;
      cairo_new_path(cr);
      if (on) {
        cairo_set_source_rgba(cr, rr, gg, bb, 1.0);
        cairo_arc(cr, dx, dy, 5.0, 0, 2 * M_PI);
        cairo_fill(cr);
      } else {
        cairo_set_source_rgba(cr, rr, gg, bb, 0.9);
        cairo_set_line_width(cr, 1.4);
        cairo_arc(cr, dx, dy, 5.0, 0, 2 * M_PI);
        cairo_stroke(cr);
      }
    }

    ry += kMenuRowH;
  }
}

} // namespace

void context_menu_popup_bounds(AppState& app, int& x, int& y, int& w, int& h) {
  const ContextMenuGeometry m = context_menu_geometry(app);
  x = m.cm_x;
  y = m.cm_y;
  w = m.cm_w;
  h = m.cm_h;
}

void context_submenu_popup_bounds(AppState& app, int& x, int& y, int& w, int& h) {
  if (app.context_menu_hover < 0 ||
      static_cast<size_t>(app.context_menu_hover) >=
          app.context_menu_items.size() ||
      app.context_menu_items[static_cast<size_t>(app.context_menu_hover)]
          .sub_items.empty()) {
    x = y = w = h = 0;
    return;
  }
  const SubmenuGeometry s = submenu_geometry(app, app.context_menu_hover);
  x = s.sub_x;
  y = s.sub_y;
  w = s.sm_w;
  h = s.sm_h;
}

void draw_context_menu(AppState& app, cairo_t* cr) {
  const ContextMenuGeometry g = context_menu_geometry(app);
  draw_menu_card(app, cr, g.cm_x, g.cm_y, g.cm_w, g.cm_h,
                 app.context_menu_opacity_pct / 100.0);
  draw_menu_rows(app, cr, app.context_menu_items, g.cm_x, g.cm_y, g.cm_w,
                 app.context_menu_hover, false);
}

void draw_context_submenu(AppState& app, cairo_t* cr) {
  if (app.context_menu_hover < 0 ||
      static_cast<size_t>(app.context_menu_hover) >=
          app.context_menu_items.size())
    return;
  const auto& item = app.context_menu_items[app.context_menu_hover];
  if (item.sub_items.empty()) return;
  const SubmenuGeometry sg = submenu_geometry(app, app.context_menu_hover);
  draw_menu_card(app, cr, sg.sub_x, sg.sub_y, sg.sm_w, sg.sm_h,
                 app.context_menu_opacity_pct / 100.0);
  draw_menu_rows(app, cr, item.sub_items, sg.sub_x, sg.sub_y, sg.sm_w,
                 app.context_menu_sub_hover, true);
}

int hit_test_context_menu(AppState& app, int x, int y) {
  // Resolved through the retained hit registry (row rects stored during
  // paint); the row-walk math is gone.
  if (!app.context_menu_open) return -1;
  const uint32_t hid = app.hit_main.query(x, y);
  if ((hid & hui::Hit::kGroupMask) != hui::Hit::kMenu) return -1;
  const int menu = hui::Hit::menu_id(hid);
  const int row = hui::Hit::menu_row(hid);
  if (menu == hui::Hit::kMenuCtxSub) {
    if (app.context_menu_hover < 0 ||
        static_cast<size_t>(app.context_menu_hover) >= app.context_menu_items.size() ||
        app.context_menu_items[app.context_menu_hover].sub_items.empty())
      return -1;
    const auto& subs = app.context_menu_items[app.context_menu_hover].sub_items;
    if (row < 0 || static_cast<size_t>(row) >= subs.size()) return -1;
    if (subs[row].action == AppState::ContextMenuAction::Separator) return -1;
    return -10 - row;
  }
  if (menu != hui::Hit::kMenuCtx) return -1;
  if (row < 0 || static_cast<size_t>(row) >= app.context_menu_items.size()) return -1;
  return row;
}

} // namespace eh::file_browser