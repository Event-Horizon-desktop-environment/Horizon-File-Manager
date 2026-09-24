#include "../app.hpp"
#include "../trace.hpp"
#include "../features/sidebar/sidebar.hpp"
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

// ── context menu drawing ─────────────────────────────────────────

namespace {

constexpr int kMenuRowH = 34;
constexpr int kMenuSepH = 9;

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
  int cm_x = 0, cm_y = 0, cm_w = 240, cm_h = 0;
};

ContextMenuGeometry context_menu_geometry(AppState& app) {
  ContextMenuGeometry g;
  g.cm_x = app.context_menu_x;
  g.cm_y = app.context_menu_y;
  g.cm_h = menu_items_height(app.context_menu_items);
  clamp_popup_rect(app, g.cm_x, g.cm_y, g.cm_w, g.cm_h);
  return g;
}

struct SubmenuGeometry {
  int sub_x = 0, sub_y = 0, sm_w = 200, sm_h = 0;
};

SubmenuGeometry submenu_geometry(AppState& app, int hover) {
  SubmenuGeometry g;
  const auto& items = app.context_menu_items;
  const ContextMenuGeometry m = context_menu_geometry(app);
  g.sm_h = menu_items_height(items[static_cast<size_t>(hover)].sub_items);
  g.sub_x = m.cm_x + m.cm_w + 3;
  if (g.sub_x + g.sm_w > app.width - 8) g.sub_x = m.cm_x - g.sm_w - 3;
  g.sub_y = m.cm_y;
  for (int j = 0; j < hover; ++j) {
    g.sub_y += (items[static_cast<size_t>(j)].action == AppState::ContextMenuAction::Separator &&
                items[static_cast<size_t>(j)].sub_items.empty())
                   ? kMenuSepH
                   : kMenuRowH;
  }
  clamp_popup_rect(app, g.sub_x, g.sub_y, g.sm_w, g.sm_h);
  return g;
}

} // namespace

static void draw_submenu_popup(AppState& app, cairo_t* cr, const std::vector<AppState::ContextMenuItem>& items,
                                int px, int py, int* out_w, int* out_h) {
  int sm_w = 200;
  int sm_h = 0;
  for (const auto& item : items) {
    sm_h += (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) ? 9 : 34;
  }

  int sm_x = px;
  int sm_y = py;
  if (sm_y + sm_h > app.height - 8) sm_y = app.height - sm_h - 8;
  if (sm_y < 8) sm_y = 8;

  draw_dialog_card(app, cr, sm_x, sm_y, sm_w, sm_h, 12);

  int ry = sm_y;
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    if (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) {
      app.hit_main.add(hui::Hit::menu(hui::Hit::kMenuCtxSub, static_cast<int>(i)), sm_x, ry, sm_w, 9);
      ry += 4;
      hui::design::hairline(cr, app, sm_x, ry, sm_w);
      ry += 5;
      continue;
    }

    int row_h = 34;
    bool hovered = (static_cast<int>(i) == app.context_menu_sub_hover);

    app.hit_main.add(hui::Hit::menu(hui::Hit::kMenuCtxSub, static_cast<int>(i)), sm_x, ry, sm_w, row_h);

    // Hover highlight
    if (hovered) {
      hui::design::row_hover(cr, app, sm_x + 6, ry + 2, sm_w - 12, row_h - 4);
    }

    // Label
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    std::string shown = hui::design::clip_end(cr, item.label, sm_w - 32);
    cairo_text_extents_t te;
    cairo_text_extents(cr, shown.c_str(), &te);
    double tx = sm_x + 16;
    double ty = ry + (row_h - te.height) / 2.0 - te.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, shown.c_str());
    ry += row_h;
  }

  if (out_w) *out_w = sm_w;
  if (out_h) *out_h = sm_h;
}

void draw_context_menu(AppState& app, cairo_t* cr) {
  const ContextMenuGeometry g = context_menu_geometry(app);
  int cm_x = g.cm_x;
  int cm_y = g.cm_y;
  int cm_w = g.cm_w;
  int cm_h = g.cm_h;

  draw_dialog_card(app, cr, cm_x, cm_y, cm_w, cm_h, 12);

  int ry = cm_y;
  for (int i = 0; i < static_cast<int>(app.context_menu_items.size()); ++i) {
    const auto& item = app.context_menu_items[i];
    if (item.action == AppState::ContextMenuAction::Separator && item.sub_items.empty()) {
      app.hit_main.add(hui::Hit::menu(hui::Hit::kMenuCtx, i), cm_x, ry, cm_w, 9);
      ry += 4;
      hui::design::hairline(cr, app, cm_x, ry, cm_w);
      ry += 5;
      continue;
    }

    int row_h = 34;
    bool has_sub = !item.sub_items.empty();
    bool hovered = (i == app.context_menu_hover);

    app.hit_main.add(hui::Hit::menu(hui::Hit::kMenuCtx, i), cm_x, ry, cm_w, row_h);

    // Hover highlight
    if (hovered) {
      hui::design::row_hover(cr, app, cm_x + 6, ry + 2, cm_w - 12, row_h - 4);
    }

    // Label text with proper vertical centering
    bool destructive = (item.action == AppState::ContextMenuAction::MoveToTrash ||
                        item.action == AppState::ContextMenuAction::PermanentDelete);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            has_sub ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    if (destructive) {
      cairo_set_source_rgba(cr, 0.96, 0.32, 0.32, 0.92);
    } else {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    }
    std::string shown = hui::design::clip_end(cr, item.label, cm_w - (has_sub ? 52 : 32));
    cairo_text_extents_t te;
    cairo_text_extents(cr, shown.c_str(), &te);
    double tx = cm_x + 16;
    double ty = ry + (row_h - te.height) / 2.0 - te.y_bearing;
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, shown.c_str());

    // Submenu arrow chevron
    if (has_sub) {
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, hovered ? 0.85 : 0.55);
      cairo_set_line_width(cr, 1.6);
      int ax = cm_x + cm_w - 18;
      int ay = ry + row_h / 2;
      cairo_move_to(cr, ax - 1, ay - 4);
      cairo_line_to(cr, ax + 4, ay);
      cairo_line_to(cr, ax - 1, ay + 4);
      cairo_stroke(cr);
    }

    ry += row_h;
  }

  // Draw open submenu
  if (app.context_menu_hover >= 0 &&
      static_cast<size_t>(app.context_menu_hover) < app.context_menu_items.size()) {
    const auto& item = app.context_menu_items[app.context_menu_hover];
    if (!item.sub_items.empty()) {
      const SubmenuGeometry sg = submenu_geometry(app, app.context_menu_hover);
      draw_submenu_popup(app, cr, item.sub_items, sg.sub_x, sg.sub_y, nullptr, nullptr);
    }
  }
}

// ── hit testing ──────────────────────────────────────────────────

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