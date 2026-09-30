// sidebar.cpp — every piece of file-browser sidebar logic and geometry in
// one module. Layout, fold/flap, painting, hit-testing and model refresh all
// derive the sidebar's layout width from app.sidebar_w() (0 while the sidebar
// is folded), never from the raw app.sidebar_width, so the content geometry
// and the painter can never disagree again about when the sidebar is folded
// and how much layout space it occupies.
#include "app/file_browser/app.hpp"
#include "app/file_browser/features/accounts/accounts.hpp"
#include "app/file_browser/features/drive/drive.hpp"
#include "app/file_browser/features/sidebar/sidebar.hpp"
#include "app/file_browser/trace.hpp"
#include "ui/hit.hpp"
#include "ui/design.hpp"

#include "services/udisks2/drive_filter.hpp"
#include "services/udisks2/udisks2_drive_service.hpp"

#include <cairo/cairo.h>

#include <mntent.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

namespace {
// Native Drive rows (Kind::Drive, "drive:" id) live in the NETWORK section:
// they need no mount step and navigating is a single click.
bool is_cloud_row(const eh::file_browser::SidebarLocation& loc) {
  return loc.kind == eh::file_browser::SidebarLocation::Kind::Drive &&
         loc.drive_id.rfind("drive:", 0) == 0;
}
} // namespace

namespace eh::file_browser {

SidebarLayout sidebar_layout(const std::vector<SidebarLocation>& locs) {
  SidebarLayout l;
  l.total = static_cast<int>(locs.size());
  while (l.places_end < l.total &&
         locs[l.places_end].kind != SidebarLocation::Kind::Favorite &&
         locs[l.places_end].kind != SidebarLocation::Kind::Network &&
         locs[l.places_end].kind != SidebarLocation::Kind::Remote &&
         locs[l.places_end].kind != SidebarLocation::Kind::Root &&
         locs[l.places_end].kind != SidebarLocation::Kind::Drive)
    ++l.places_end;
  l.fav_start = l.places_end;
  while (l.fav_start < l.total &&
         locs[l.fav_start].kind == SidebarLocation::Kind::Favorite)
    ++l.fav_start;
  l.network_end = l.fav_start;
  while (l.network_end < l.total &&
         (locs[l.network_end].kind == SidebarLocation::Kind::Network ||
          locs[l.network_end].kind == SidebarLocation::Kind::Remote ||
          is_cloud_row(locs[l.network_end])))
    ++l.network_end;
  l.drives_start = l.network_end;
  l.searches_start = l.total;
  for (int i = l.total - 1; i >= l.drives_start; --i) {
    if (locs[static_cast<size_t>(i)].kind != SidebarLocation::Kind::SavedSearch)
      break;
    l.searches_start = i;
  }
  const double zf = kSidebarZf;
  l.header_h = static_cast<int>(24.0 * zf);
  l.div_h = static_cast<int>(16.0 * zf) + 1 + static_cast<int>(16.0 * zf);
  l.top_pad = static_cast<int>(24.0 * zf);
  // Mirror the paint order exactly: top pad, PLACES header always, then
  // each non-empty section with a divider + header first.
  int y = l.top_pad + l.header_h;
  for (int i = 0; i < l.places_end; ++i) y += sidebar_row_h(locs[i]);
  if (l.fav_start > l.places_end) {
    y += l.div_h + l.header_h;
    for (int i = l.places_end; i < l.fav_start; ++i) y += sidebar_row_h(locs[i]);
  }
  if (l.network_end > l.fav_start) {
    y += l.div_h + l.header_h;
    for (int i = l.fav_start; i < l.network_end; ++i) y += sidebar_row_h(locs[i]);
  }
  if (l.total > l.drives_start) {
    y += l.div_h + l.header_h;
    for (int i = l.drives_start; i < l.searches_start; ++i) y += sidebar_row_h(locs[i]);
  }
  if (l.total > l.searches_start) {
    y += l.div_h + l.header_h;
    for (int i = l.searches_start; i < l.total; ++i) y += sidebar_row_h(locs[i]);
  }
  l.content_h = y;
  return l;
}

int SidebarLayout::row_y(const std::vector<SidebarLocation>& locs,
                         int idx) const {
  int y = top_pad + header_h;
  for (int i = 0; i < places_end; ++i) {
    if (i == idx) return y;
    y += sidebar_row_h(locs[i]);
  }
  if (fav_start > places_end) {
    y += div_h + header_h;
    for (int i = places_end; i < fav_start; ++i) {
      if (i == idx) return y;
      y += sidebar_row_h(locs[i]);
    }
  }
  if (network_end > fav_start) {
    y += div_h + header_h;
    for (int i = fav_start; i < network_end; ++i) {
      if (i == idx) return y;
      y += sidebar_row_h(locs[i]);
    }
  }
  if (total > drives_start) {
    y += div_h + header_h;
    for (int i = drives_start; i < searches_start; ++i) {
      if (i == idx) return y;
      y += sidebar_row_h(locs[i]);
    }
  }
  if (total > searches_start) {
    y += div_h + header_h;
    for (int i = searches_start; i < total; ++i) {
      if (i == idx) return y;
      y += sidebar_row_h(locs[i]);
    }
  }
  return y;
}

} // namespace eh::file_browser
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace eh::file_browser {

namespace {

// `desktop_dir` / `xdg_user_dir`: XDG helpers for the sidebar's Places rows.
static std::string desktop_dir() {
  auto h = home_dir();
  return h + "/Desktop";
}

static std::string xdg_user_dir(const char* env, const char* fallback) {
  if (auto* e = std::getenv(env)) return e;
  return home_dir() + "/" + fallback;
}

// Whether the system trash contains anything (drives the full/empty icon).
// Cached with a 1 s TTL: called per trash row per frame, and a directory
// scan per frame is pure waste for state that changes on user action.
static bool trash_has_files() {
  using clock = std::chrono::steady_clock;
  static bool cached = false;
  static clock::time_point expires{};
  auto now = clock::now();
  if (now < expires) return cached;
  expires = now + std::chrono::seconds(1);
  const char* home = std::getenv("HOME");
  if (!home) return (cached = false);
  fs::path trash_files = fs::path(home) / ".local/share/Trash/files";
  std::error_code ec;
  fs::directory_iterator it(trash_files, ec);
  if (ec) return (cached = false);
  for (auto& entry : it) {
    (void)entry;
    return (cached = true);
  }
  return (cached = false);
}

}  // namespace

// Byte-size formatter ("463.2 GB"), shared by the sidebar's drive usage rows
// and draw.cpp's list/status views (declared in app.hpp).
std::string format_size(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
  if (bytes < 1024ULL * 1024 * 1024)
    return std::to_string(bytes / (1024 * 1024)) + " MB";
  return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
}


void size_sidebar_to_content(AppState& app, cairo_t* cr) {
  if (!app.sidebar_expanded || app.sidebar_locations.empty()) return;
  constexpr double kSbZf = 1.2; // must match draw_sidebar's pinned scale
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0 * kSbZf);
  double label_x = (16 + 20 + 12) * kSbZf;
  double widest = 0;
  for (const auto& loc : app.sidebar_locations) {
    cairo_text_extents_t te;
    cairo_text_extents(cr, loc.label.c_str(), &te);
    widest = std::max(widest, static_cast<double>(te.x_advance));
  }
  // icon + gap + widest label + mount-indicator/usage reserve + edge padding
  int needed = static_cast<int>(label_x + widest + 40.0 * kSbZf + 16.0 * kSbZf);
  // Leave room for the info panel and a usable content column so the two can
  // never combine into an overlap on narrow windows. The ops panel is an
  // overlay, so it does not reserve content width.
  int reserve = (app.info_panel_open ? app.info_panel_width : 0);
  int cap = std::max(160, std::min(std::max(340, app.width * 3 / 5),
                                   app.width - reserve - 240));
  int want = std::max(app.sidebar_width, needed);
  app.sidebar_width = std::min(want, cap);
}

void draw_sidebar(AppState& app, cairo_t* cr, int sidebar_w, int top_y,
                  int) {
  // One-shot: log first-paint sub-phases to find cold-start hot spots.
  struct SubMark { const char* n; };
  auto sub_t0 = std::chrono::steady_clock::now();
  auto sub = [&, tag = ""](const char* n) mutable {
    static int calls = 0;
    if (calls++ > 6) return;   // only the first paint's marks
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - sub_t0).count();
    if (trace::enabled().load(std::memory_order_relaxed))
      trace::log("SIDEBAR FIRST %s %.2f ms", n, ms);
    sub_t0 = std::chrono::steady_clock::now();
    (void)tag;
  };
  double zf = app.zoom_pct / 100.0;
  // Single source of truth for sections + content height (scroll clamp,
  // hit-testing and paint below all derive from it).
  const SidebarLayout layout = sidebar_layout(app.sidebar_locations);
  const int total = layout.total;
  const int places_end = layout.places_end;
  const int fav_start = layout.fav_start;
  const int network_end = layout.network_end;
  const int drives_start = layout.drives_start;
  const int searches_start = layout.searches_start;

  // Scroll clamp against the exact painted extent.
  {
    int available = app.height - app.top_bar_height - app.tab_bar_height - app.status_bar_height;
    app.sidebar_content_h = layout.content_h;
    // Clamp scroll to prevent blank space below last item
    int max_scroll = std::max(0, layout.content_h - available);
    if (app.sidebar_scroll_px > max_scroll)
      app.sidebar_scroll_px = max_scroll;
  }
  zf = 1.2;

  // HTML has py-6 (24px) padding at top of sidebar
  int y = top_y - app.sidebar_scroll_px + static_cast<int>(24.0 * zf);

  auto draw_item = [&](int idx) {
    const auto& loc = app.sidebar_locations[idx];
    // Row heights come from the shared layout helpers (zf is pinned to
    // kSidebarZf above), so paint can never drift from the scroll clamp.
    int item_h = sidebar_row_h(loc);
    bool hovered = (idx == app.sidebar_hover_idx);
    bool drop_target = app.drop_target_is_sidebar && idx == app.drop_target_sidebar_idx;

    int sb_margin = static_cast<int>(8.0 * zf); // px-4 in HTML

    // Retained hit region (single source of truth for input; the rect is
    // the row's full width so edge-to-edge clicks work — see hit_registry).
    app.hit_main.add(hui::Hit::sidebar_row(idx), 0, y, sidebar_w, item_h);

    if (drop_target) {
      double pulse = 0.16 + 0.06 * std::sin(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count() * 0.006);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, pulse);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h,
                        static_cast<int>(12.0 * zf)); // rounded-2xl
      cairo_fill(cr);
      app.pendingRedraw = true;
    } else if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.14);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h,
                        static_cast<int>(12.0 * zf)); // rounded-2xl
      cairo_fill(cr);
    }

    // Icon (w-5 = 20px)
    int icon_sz = static_cast<int>(20.0 * zf);
    int icon_x = static_cast<int>(16.0 * zf); // px-4

    cairo_surface_t* cs = nullptr;
    switch (loc.kind) {
      case SidebarLocation::Kind::Desktop:   cs = app.icon_desktop_svg; break;
      case SidebarLocation::Kind::Documents: cs = app.icon_documents_svg; break;
      case SidebarLocation::Kind::Downloads: cs = app.icon_downloads_svg; break;
      case SidebarLocation::Kind::Music:     cs = app.icon_music_svg; break;
      case SidebarLocation::Kind::Pictures:  cs = app.icon_pictures_svg; break;
      case SidebarLocation::Kind::Videos:    cs = app.icon_videos_svg; break;
      default: break;
    }

    auto draw_icon_at = [&](int ix, int iy) {
      const char* icon_name = loc.icon_name.c_str();
      std::string trash_icon;
      if (loc.kind == SidebarLocation::Kind::Trash) {
        trash_icon = trash_has_files() ? "user-trash-full" : "user-trash";
        icon_name = trash_icon.c_str();
      }
      // Theme-first: honor the system icon pack (e.g. MacTahoe). The bundled
      // monochrome stencils below are only a fallback for themes that lack
      // these names — they used to take precedence, painting flat
      // text-colored glyphs no matter which icon theme was active.
      //
      // Async resolve: never decode theme SVGs on the paint thread.
      // The placeholder letter shows for a frame or two, then the real
      // icon pops in (catch-up frames are scheduled right after startup).
      const auto* ic = [&]{
        auto ti0 = std::chrono::steady_clock::now();
        const auto* r = app.icons.tray_icon(icon_name, icon_sz);
        static std::atomic<int> tic{0};
        if (tic.fetch_add(1, std::memory_order_relaxed) < 12 &&
            trace::enabled().load(std::memory_order_relaxed))
          trace::log("SIDEBAR tray[%s] %.2f ms", icon_name,
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - ti0).count());
        return r;
      }();
      if (ic && ic->surface) {
        double iw = static_cast<double>(ic->width);
        double ih = static_cast<double>(ic->height);
        double scale = icon_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_translate(cr, ix, iy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, ic->surface, (icon_sz / scale - iw) / 2,
                                  (icon_sz / scale - ih) / 2);
        cairo_paint(cr);
        cairo_restore(cr);
        return;
      }
      if (cs) {
        double iw = static_cast<double>(cairo_image_surface_get_width(cs));
        double ih = static_cast<double>(cairo_image_surface_get_height(cs));
        double scale = icon_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_translate(cr, ix, iy);
        cairo_scale(cr, scale, scale);
        cairo_rectangle(cr, 0, 0, icon_sz / scale, icon_sz / scale);
        cairo_clip(cr);
        cairo_mask_surface(cr, cs, (icon_sz / scale - iw) / 2, (icon_sz / scale - ih) / 2);
        cairo_restore(cr);
        return;
      }
      cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 16.0 * zf);
      cairo_move_to(cr, ix, iy + icon_sz);
      char fallback[2] = { loc.label.empty() ? '?' : loc.label[0], '\0' };
      cairo_show_text(cr, fallback);
    };

    // For items with extra usage row, center icon in the main row (top 36px)
    int main_row_h = static_cast<int>(36.0 * zf);
    {
      auto mi0 = std::chrono::steady_clock::now();
      draw_icon_at(icon_x, y + (main_row_h - icon_sz) / 2);
      static std::atomic<int> mic{0};
      if (mic.fetch_add(1, std::memory_order_relaxed) < 12 &&
          trace::enabled().load(std::memory_order_relaxed)) {
        trace::log("SIDEBAR icon[%s] %.2f ms", loc.label.c_str(),
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - mi0)
                       .count());
      }
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0 * zf); // text-sm = 14px, close enough
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);

    // gap-3 = 12px between icon and label
    int label_x = icon_x + icon_sz + static_cast<int>(12.0 * zf);

    bool is_drive_kind =
        loc.kind == SidebarLocation::Kind::Drive ||
        loc.kind == SidebarLocation::Kind::Root;
    // Cloud rows have no mount indicator; other drives reserve room for it.
    bool cloud = is_cloud_row(loc);
    // Drives leave room for the mount indicator; other rows a small margin.
    int max_label_w = sidebar_w - label_x -
                      static_cast<int>((is_drive_kind && !cloud ? 40.0 : 14.0) * zf);
    std::string shown = hui::design::clip_end(cr, loc.label, max_label_w);
    cairo_move_to(cr, label_x,
                  y + main_row_h / 2 + static_cast<int>(4.0 * zf));
    cairo_show_text(cr, shown.c_str());

    // Mount indicator for drives (never for cloud rows: no mount step,
    // so the eject icon would be a dead end).
    if (loc.kind == SidebarLocation::Kind::Drive && !is_cloud_row(loc)) {
      if (loc.is_mounted && app.mounted_svg) {
        int ind_sz = static_cast<int>(18.0 * zf);
        int ind_x = sidebar_w - static_cast<int>(24.0 * zf);
        int ind_y = y + (main_row_h - ind_sz) / 2;
        // Hover background
        if (idx == app.sidebar_mount_hover_idx) {
          cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                                 app.text_secondary_b, 0.12);
          double rad = ind_sz * 0.5 + 4;
          cairo_new_path(cr); // standalone disc
          cairo_arc(cr, ind_x + ind_sz / 2.0, ind_y + ind_sz / 2.0, rad, 0, 2 * M_PI);
          cairo_fill(cr);
        }
        double iw = static_cast<double>(cairo_image_surface_get_width(app.mounted_svg));
        double ih = static_cast<double>(cairo_image_surface_get_height(app.mounted_svg));
        double scale = ind_sz / std::max(1.0, std::max(iw, ih));
        cairo_save(cr);
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
        cairo_rectangle(cr, ind_x, ind_y, ind_sz, ind_sz);
        cairo_clip(cr);
        cairo_translate(cr, ind_x, ind_y);
        cairo_scale(cr, scale, scale);
        cairo_translate(cr, (ind_sz / scale - iw) / 2, (ind_sz / scale - ih) / 2);
        cairo_mask_surface(cr, app.mounted_svg, 0, 0);
        cairo_restore(cr);
      }
    }

    // Usage progress bar for drives with data
    if (sidebar_has_usage(loc)) {
      int usage_y = y + main_row_h;
      int bar_x = icon_x;
      int bar_w = sidebar_w - bar_x - static_cast<int>(12.0 * zf);
      int bar_h = static_cast<int>(4.0 * zf);
      int bar_y = usage_y + static_cast<int>(10.0 * zf);

      // Usage text (elided to the bar width)
      uint64_t used = loc.total_bytes - loc.free_bytes;
      cairo_set_font_size(cr, 10.0 * zf);
      std::string usage_text = hui::design::clip_end(
          cr, format_size(used) + " / " + format_size(loc.total_bytes), bar_w);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 0.8);
      cairo_move_to(cr, bar_x, bar_y - static_cast<int>(4.0 * zf));
      cairo_show_text(cr, usage_text.c_str());

      double frac = std::min(1.0, static_cast<double>(used) /
                                   static_cast<double>(loc.total_bytes));

      hui::design::bar(cr, app, bar_x, bar_y, bar_w, frac, static_cast<double>(bar_h));
    }

    y += item_h;
  };

  auto draw_header = [&](const char* text) {
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                           app.text_secondary_b, 1.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.0 * zf); // text-xs
    cairo_move_to(cr, static_cast<int>(16.0 * zf), y + static_cast<int>(14.0 * zf));
    cairo_show_text(cr, text);
    y += static_cast<int>(24.0 * zf);
  };

  auto draw_divider = [&]() {
    y += static_cast<int>(16.0 * zf);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.15);
    int div_margin = static_cast<int>(16.0 * zf);
    cairo_rectangle(cr, div_margin, y, sidebar_w - div_margin * 2, 1);
    cairo_fill(cr);
    y += 1 + static_cast<int>(16.0 * zf);
  };

  sub("pre");
  draw_header("PLACES");
  for (int i = 0; i < std::min(places_end, total); ++i)
    draw_item(i);

  sub("places");
  if (fav_start > places_end) {
    draw_divider();
    draw_header("FAVORITES");

    // Draw "drop to add" indicator when dragging over the section
    bool fav_hover = app.drop_target_fav_section;

    int item_h = static_cast<int>(36.0 * zf);
    bool dragging = app.sidebar_fav_dragging;
    int drag_sb_idx = dragging ? places_end + app.sidebar_fav_drag_from : -1;

    for (int i = places_end; i < std::min(fav_start, total); ++i) {
      // Draw insertion line before this item if slot matches
      if (dragging) {
        int slot = i - places_end;
        if (slot == app.sidebar_fav_drag_to_visual && slot != app.sidebar_fav_drag_from) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.6);
          cairo_set_line_width(cr, 2.0);
          int div_margin = static_cast<int>(16.0 * zf);
          cairo_move_to(cr, div_margin, y);
          cairo_line_to(cr, sidebar_w - div_margin, y);
          cairo_stroke(cr);
        }
      }
      if (dragging && i == drag_sb_idx) {
        // Draw the dragged item dimmed
        cairo_push_group(cr);
        draw_item(i);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 0.35);
      } else {
        draw_item(i);
      }
    }
    // Insertion line at the end of the list
    if (dragging) {
      int last_slot = fav_start - places_end;
      if (last_slot == app.sidebar_fav_drag_to_visual && last_slot != app.sidebar_fav_drag_from) {
        cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.6);
        cairo_set_line_width(cr, 2.0);
        int div_margin = static_cast<int>(16.0 * zf);
        cairo_move_to(cr, div_margin, y);
        cairo_line_to(cr, sidebar_w - div_margin, y);
        cairo_stroke(cr);
      }
    }

    // Ghost — translucent preview of the dragged item following the cursor
    if (dragging && drag_sb_idx >= places_end && drag_sb_idx < fav_start) {
      int ghost_y = app.sidebar_fav_drag_current_y - item_h / 2;
      int saved_y = y;
      y = ghost_y;
      // Accent background
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
      int sb_margin = static_cast<int>(6.0 * zf);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h, sb_margin);
      cairo_fill(cr);
      // Accent border
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.5);
      cairo_set_line_width(cr, 1.5);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h, sb_margin);
      cairo_stroke(cr);
      // Item content
      cairo_push_group(cr);
      draw_item(drag_sb_idx);
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.85);
      y = saved_y;
    }

    // If dragging over Favorites (external drop), draw a "+" indicator at the bottom
    if (fav_hover) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.20);
      int sb_margin = static_cast<int>(6.0 * zf);
      draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h, sb_margin);
      cairo_fill(cr);
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 18.0 * zf);
      cairo_move_to(cr, static_cast<int>(16.0 * zf), y + item_h / 2 + static_cast<int>(6.0 * zf));
      cairo_show_text(cr, "+");
      cairo_move_to(cr, static_cast<int>(38.0 * zf), y + item_h / 2 + static_cast<int>(5.0 * zf));
      cairo_set_font_size(cr, 13.0 * zf);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.7);
      cairo_show_text(cr, "Add to Favorites");
      y += item_h;
    }
  } else if (app.drop_target_fav_section) {
    // Empty Favorites section — draw placeholder indicator
    draw_divider();
    draw_header("FAVORITES");
    int item_h = static_cast<int>(36.0 * zf);
    app.hit_main.add(hui::Hit::kSidebarFavAdd, 0, y, sidebar_w, item_h);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.20);
    int sb_margin = static_cast<int>(6.0 * zf);
    draw_rounded_rect(cr, sb_margin, y, sidebar_w - sb_margin * 2, item_h, sb_margin);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.7);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 18.0 * zf);
    cairo_move_to(cr, static_cast<int>(16.0 * zf), y + item_h / 2 + static_cast<int>(6.0 * zf));
    cairo_show_text(cr, "+");
    cairo_move_to(cr, static_cast<int>(38.0 * zf), y + item_h / 2 + static_cast<int>(5.0 * zf));
    cairo_set_font_size(cr, 13.0 * zf);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.7);
    cairo_show_text(cr, "Add to Favorites");
    y += item_h;
  }

  sub("favorites");
  if (network_end > fav_start) {
    draw_divider();
    draw_header("NETWORK");
    for (int i = fav_start; i < std::min(network_end, total); ++i)
      draw_item(i);
  }

  sub("network");
  if (searches_start > drives_start) {
    draw_divider();
    draw_header("DRIVES");
    for (int i = drives_start; i < std::min(searches_start, total); ++i)
      draw_item(i);
  }

  sub("drives");
  if (total > searches_start) {
    draw_divider();
    draw_header("SEARCHES");
    for (int i = searches_start; i < total; ++i)
      draw_item(i);
  }

  sub("searches");

  // the content overflows the panel. Wheel scrolls (input/scroll.cpp);
  // the thumb drags (begin_sidebar_scroll in input).
  {
    int panel_h = app.height - app.status_bar_height - top_y;
    int max_scroll = std::max(0, app.sidebar_content_h - panel_h);
    app.sidebar_scroll_max = max_scroll;
    if (max_scroll > 0 && sidebar_w > 16 && panel_h > 40) {
      int track_x = sidebar_w - 8;
      int track_y = top_y + 4;
      int track_h = std::max(1, panel_h - 8);
      int thumb_h =
          std::max(28, track_h * panel_h / app.sidebar_content_h);
      if (thumb_h > track_h) thumb_h = track_h;
      int thumb_y =
          track_y + (track_h - thumb_h) * app.sidebar_scroll_px / max_scroll;
      app.sidebar_scroll_track[0] = track_x - 3;
      app.sidebar_scroll_track[1] = track_y;
      app.sidebar_scroll_track[2] = 10;
      app.sidebar_scroll_track[3] = track_h;
      app.sidebar_scroll_thumb[0] = track_x;
      app.sidebar_scroll_thumb[1] = thumb_y;
      app.sidebar_scroll_thumb[2] = 4;
      app.sidebar_scroll_thumb[3] = thumb_h;
      bool active = app.sidebar_scroll_dragging ||
                    (app.pointerX >= track_x - 3 &&
                     app.pointerX < track_x + 7 &&
                     app.pointerY >= thumb_y &&
                     app.pointerY < thumb_y + thumb_h);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, active ? 0.75 : 0.4);
      draw_rounded_rect(cr, track_x, thumb_y, 4, thumb_h, 2);
      cairo_fill(cr);
    } else {
      app.sidebar_scroll_track[2] = 0;
      app.sidebar_scroll_thumb[3] = 0;
    }
  }
}

int hit_test_sidebar(AppState& app, int x, int y) {
  // Resolved through the retained hit registry (rects stored during paint),
  // never re-derived here: sidebar geometry cannot drift (see
  // ui/hit.hpp). Only the chrome bounds below are checked
  // directly; rows come from the registry.
  int side_w = app.effective_sidebar_width();
  if (side_w <= 0) return -1;
  if (x < 0 || x >= side_w) return -1;
  int top = (app.sidebar_folded && app.sidebar_folded_revealed)
                ? app.content_top_y()
                : 0;
  int y0 = y - top;
  if (y0 < static_cast<int>(24.0 * 1.2) ||
      y0 >= app.height - app.status_bar_height - top)
    return -1;

  uint32_t hid = app.hit_main.query(x, y);
  if ((hid & hui::Hit::kGroupMask) != hui::Hit::kSidebarRow) return -1;
  int idx = static_cast<int>(hid & hui::Hit::kIndexMask);
  if (idx < 0 || idx >= static_cast<int>(app.sidebar_locations.size()))
    return -1;
  return idx;
}
bool hit_test_fav_section(AppState& app, int x, int y) {
  int side_w = app.effective_sidebar_width();
  if (side_w <= 0) return false;
  if (x < 0 || x >= side_w) return false;
  int top = (app.sidebar_folded && app.sidebar_folded_revealed)
                ? app.content_top_y()
                : 0;
  int y0 = y - top;
  if (y0 < static_cast<int>(24.0 * 1.2) ||
      y0 >= app.height - app.status_bar_height - top)
    return false;

  const SidebarLayout layout = sidebar_layout(app.sidebar_locations);
  const int total = layout.total;
  const int places_end = layout.places_end;
  const int fav_start = layout.fav_start;
  const int network_end = layout.network_end;

  const int drives_start = network_end;
  int fav_count = fav_start - places_end;
  if (fav_count == 0 && network_end >= total) return false;

  int padding = layout.top_pad;
  int header_h = layout.header_h;
  int item_h = static_cast<int>(36.0 * kSidebarZf);
  int div_total = layout.div_h;

  int rel_y = y0 + app.sidebar_scroll_px;

  // Skip padding + PLACES header + items — Favorites section starts at the divider
  int places_bottom = padding + header_h + places_end * item_h;

  // Favorites items area: from the divider after Places to end of last fav item
  if (fav_count > 0) {
    int fav_end = places_bottom + div_total + header_h + fav_count * item_h;
    return rel_y >= places_bottom && rel_y < fav_end;
  }

  // No favorites — the "Add to Favorites" zone is the divider area after Places
  return rel_y >= places_bottom && rel_y < places_bottom + div_total;
}
void refresh_sidebar(AppState& app) {
  app.sidebar_locations.clear();
  std::string home = home_dir();

  auto add_location = [&](SidebarLocation::Kind kind, const char* label,
                           const std::string& path, const char* icon) {
    SidebarLocation loc;
    loc.kind = kind;
    loc.label = label;
    loc.path = path;
    loc.icon_name = icon;
    app.sidebar_locations.push_back(std::move(loc));
  };

  add_location(SidebarLocation::Kind::Computer, "My Computer", "computer://", "computer");
  if (app.recent_enabled)
    add_location(SidebarLocation::Kind::Recent, "Recent", "recent://", "document-open-recent");
  add_location(SidebarLocation::Kind::Starred, "Starred", "starred://", "starred");

  add_location(SidebarLocation::Kind::Home, "Home", home, "user-home");
  add_location(SidebarLocation::Kind::Desktop, "Desktop", desktop_dir(), "user-desktop");
  add_location(SidebarLocation::Kind::Documents, "Documents",
               xdg_user_dir("XDG_DOCUMENTS_DIR", "Documents"), "folder-documents");
  add_location(SidebarLocation::Kind::Downloads, "Downloads",
               xdg_user_dir("XDG_DOWNLOAD_DIR", "Downloads"), "folder-download");
  add_location(SidebarLocation::Kind::Pictures, "Pictures",
               xdg_user_dir("XDG_PICTURES_DIR", "Pictures"), "folder-pictures");
  add_location(SidebarLocation::Kind::Music, "Music",
               xdg_user_dir("XDG_MUSIC_DIR", "Music"), "folder-music");
  add_location(SidebarLocation::Kind::Videos, "Videos",
               xdg_user_dir("XDG_VIDEOS_DIR", "Videos"), "folder-videos");
  add_location(SidebarLocation::Kind::Trash, "Trash",
               home + "/.local/share/Trash/files", "user-trash");

  for (const auto& fav_path : app.favorites) {
    std::string label = fs::path(fav_path).filename().string();
    if (label.empty()) label = fav_path;
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Favorite;
    loc.label = label;
    loc.path = fav_path;
    loc.icon_name = "folder";
    app.sidebar_locations.push_back(std::move(loc));
  }

  add_location(SidebarLocation::Kind::Network, "Network", "network://",
               "folder-remote");

  for (const auto& srv : app.remote_servers) {
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Remote;
    loc.label = (srv.user.empty() ? "" : srv.user + "@") + srv.host;
    loc.path = build_sftp_uri(srv.host, srv.user, srv.port, srv.path);
    loc.icon_name = "folder-remote";
    app.sidebar_locations.push_back(std::move(loc));
  }


  // Build device → mountpoint map from /proc/mounts (needed for root label + drive filtering)
  std::map<std::string, std::string> mount_map;
  {
    auto* f = setmntent("/proc/mounts", "r");
    if (f) {
      struct mntent* mnt;
      while ((mnt = getmntent(f)) != nullptr) {
        std::string dev = mnt->mnt_fsname;
        if (dev.size() >= 5 && dev.substr(0, 5) == "/dev/")
          mount_map[dev] = mnt->mnt_dir;
      }
      endmntent(f);
    }
  }

  // Build a map of device path → UDisks2 info (if available).
  // This gives us drive_id for mount/unmount and accurate mount status.
  auto& udisks = drives::UDisks2DriveService::instance();
  auto udisks_drives = udisks.query_drives();
  std::map<std::string, const drives::DriveInfo*> udisk_map;
  for (const auto& d : udisks_drives)
    udisk_map[d.device] = &d;

  std::set<std::string> seen_devs;
  std::vector<SidebarLocation> drive_locs;

  auto add_drive = [&](const std::string& label, const std::string& dev) {
    if (!seen_devs.insert(dev).second) return;

    // Determine mount point and fs type for filtering
    std::string mp;
    std::string fs;
    auto ui = udisk_map.find(dev);
    if (ui != udisk_map.end()) {
      mp = ui->second->mounted ? ui->second->mount_point : "";
      fs = ui->second->id_type;
    } else {
      auto mi = mount_map.find(dev);
      if (mi != mount_map.end()) mp = mi->second;
    }

    if (drives::should_hide_drive(dev, mp, fs, label)) return;

    // ISO loop devices only show while mounted; an attached-but-unmounted
    // loop (e.g. after Unmount, before Loop.Delete completes) must not linger.
    if (mp.empty() && drives::is_iso_loop_device(dev)) return;

    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Drive;
    loc.label = label;
    loc.icon_name = drives::is_iso_loop_device(dev) ? "drive-optical" : "drive-harddisk";

    if (ui != udisk_map.end()) {
      loc.drive_id = ui->second->object_path;
      loc.is_mounted = ui->second->mounted;
      loc.path = ui->second->mounted ? ui->second->mount_point : dev;
    } else {
      auto mi = mount_map.find(dev);
      loc.drive_id = dev;
      loc.is_mounted = (mi != mount_map.end());
      loc.path = loc.is_mounted ? mi->second : dev;
    }
    drive_locs.push_back(std::move(loc));
  };

  auto unescape_name = [](std::string s) {
    for (auto p = s.find("\\x20"); p != std::string::npos;
         p = s.find("\\x20", p + 1))
      s.replace(p, 4, " ");
    return s;
  };

  auto resolve_dev = [](const std::string& link_path) -> std::string {
    char buf[256];
    ssize_t len = readlink(link_path.c_str(), buf, sizeof(buf) - 1);
    if (len < 0) return {};
    buf[len] = '\0';
    std::string dev = buf;
    if (dev.size() > 6 && dev.substr(0, 6) == "../../")
      return "/dev/" + dev.substr(6);
    if (dev.size() > 3 && dev.substr(0, 3) == "../")
      return "/dev/" + dev.substr(3);
    return dev;
  };

  // Find root device and its filesystem label
  std::string root_dev;
  std::string root_label;
  // Whether root_label is a real name (vs a capacity/device fallback).
  // The root row must read as a place ("File System"), never as a bare
  // capacity like "1.8TB" — capacity already shows in the usage bar lines.
  bool root_has_name = false;
  for (auto& [dev, mp] : mount_map) {
    if (mp == "/") { root_dev = dev; break; }
  }
  if (!root_dev.empty()) {
    // Try filesystem label on the root device itself
    auto* dlabel = opendir("/dev/disk/by-label");
    if (dlabel) {
      struct dirent* entry;
      while ((entry = readdir(dlabel)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string link = resolve_dev(std::string("/dev/disk/by-label/") + entry->d_name);
        if (link == root_dev) {
          root_label = unescape_name(entry->d_name);
          break;
        }
      }
      closedir(dlabel);
    }
    // If no label on root device, try any partition on the same disk
    if (root_label.empty()) {
      std::string root_disk = drives::disk_device_from_partition(root_dev);
      auto* dlabel = opendir("/dev/disk/by-label");
      if (dlabel) {
        struct dirent* entry;
        while ((entry = readdir(dlabel)) != nullptr) {
          if (entry->d_name[0] == '.') continue;
          std::string link = resolve_dev(std::string("/dev/disk/by-label/") + entry->d_name);
          if (drives::disk_device_from_partition(link) == root_disk) {
            root_label = unescape_name(entry->d_name);
            break;
          }
        }
        closedir(dlabel);
      }
    }
    if (root_label.empty()) {
      // Try partition label from by-partlabel
      auto* plabel = opendir("/dev/disk/by-partlabel");
      if (plabel) {
        struct dirent* entry;
        while ((entry = readdir(plabel)) != nullptr) {
          if (entry->d_name[0] == '.') continue;
          std::string link = resolve_dev(std::string("/dev/disk/by-partlabel/") + entry->d_name);
          if (link == root_dev) {
            root_label = unescape_name(entry->d_name);
            break;
          }
        }
        closedir(plabel);
      }
    }
    root_has_name =
        !root_label.empty() && !drives::is_generic_partition_label(root_label);
    if (!root_has_name) {
      uint64_t size = drives::get_device_size_bytes(root_dev);
      if (size > 0)
        root_label = drives::format_device_size(size);
      else if (root_label.empty())
        root_label = root_dev.substr(5);
    }
  }
  add_location(SidebarLocation::Kind::Root,
               root_has_name ? root_label.c_str() : "File System",
               "/", "drive-harddisk");
  {
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
      auto& root = app.sidebar_locations.back();
      root.total_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_blocks;
      root.free_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_bavail;
    }
  }

  // Exclude root device from drive scans (it's already shown as the Root entry above)
  if (!root_dev.empty()) seen_devs.insert(root_dev);

  // Scan /dev/disk/by-label/ for filesystem labels
  auto* d = opendir("/dev/disk/by-label");
  if (d) {
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      if (entry->d_name[0] == '.') continue;
      std::string label = unescape_name(entry->d_name);
      std::string dev = resolve_dev(std::string("/dev/disk/by-label/") + entry->d_name);
      if (dev.empty()) continue;
      add_drive(label, dev);
    }
    closedir(d);
  }

  // Scan /dev/disk/by-partlabel/ for GPT partition names not already seen
  d = opendir("/dev/disk/by-partlabel");
  if (d) {
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      if (entry->d_name[0] == '.') continue;
      std::string part_label = unescape_name(entry->d_name);
      std::string dev = resolve_dev(std::string("/dev/disk/by-partlabel/") + entry->d_name);
      if (dev.empty()) continue;
      if (drives::should_hide_drive(dev, {}, {}, part_label)) continue;
      std::string label;
      if (drives::is_generic_partition_label(part_label)) {
        uint64_t size = drives::get_device_size_bytes(dev);
        label = size > 0 ? drives::format_device_size(size) : part_label;
      } else {
        label = part_label;
      }
      add_drive(label, dev);
    }
    closedir(d);
  }

  // Also add any UDisks2 drives that weren't found by the scanning above
  for (const auto& d : udisks_drives) {
    if (seen_devs.count(d.device)) continue;
    std::string label = d.label;
    if (label == d.device.substr(d.device.find_last_of('/') + 1))
      label.clear();
    if (label.empty() || drives::is_generic_partition_label(label)) {
      // Try partition label from by-partlabel
      auto* plabel = opendir("/dev/disk/by-partlabel");
      if (plabel) {
        struct dirent* entry;
        while ((entry = readdir(plabel)) != nullptr) {
          if (entry->d_name[0] == '.') continue;
          std::string link = resolve_dev(std::string("/dev/disk/by-partlabel/") + entry->d_name);
          if (link == d.device) {
            std::string pl = unescape_name(entry->d_name);
            if (!drives::is_generic_partition_label(pl)) {
              label = pl;
            }
            break;
          }
        }
        closedir(plabel);
      }
    }
    if (label.empty()) {
      // ISO loop without a volume label: show the image filename.
      std::string backing = drives::iso_loop_backing_file(d.device);
      if (!backing.empty()) {
        auto slash = backing.find_last_of('/');
        std::string base = (slash == std::string::npos) ? backing : backing.substr(slash + 1);
        if (!base.empty()) label = base;
      }
    }
    if (label.empty()) {
      uint64_t size = drives::get_device_size_bytes(d.device);
      label = size > 0 ? drives::format_device_size(size) : d.device;
    }
    add_drive(label, d.device);
  }

  // Sort drives: mounted first, then unmounted
  std::stable_partition(drive_locs.begin(), drive_locs.end(),
                         [](const SidebarLocation& l) { return l.is_mounted; });

  // Query disk usage for mounted drives
  for (auto& loc : drive_locs) {
    if (loc.is_mounted && !loc.path.empty()) {
      struct statvfs vfs;
      if (statvfs(loc.path.c_str(), &vfs) == 0) {
        loc.total_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_blocks;
        loc.free_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_bavail;
      }
    }
  }

  // rows stay visible above the (often tall) local-disk list. No mount
  // step — the "drive:" drive_id navigates straight in, mount/unmount
  // ignore it.
  for (auto& loc : drive_sidebar_rows(app))
    app.sidebar_locations.push_back(std::move(loc));

  for (auto& loc : drive_locs)
    app.sidebar_locations.push_back(std::move(loc));

  // other non-native mounts). Listed with the drives, Nautilus-style; the
  // "gio:" drive_id routes their mount/unmount clicks to the GIO workers.
  for (auto& vol : gio_list_network_volumes()) {
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Drive;
    loc.label = vol.name;
    loc.icon_name = vol.icon.empty() ? "folder-remote" : vol.icon;
    loc.path = vol.uri;
    loc.drive_id = vol.key;
    loc.is_mounted = vol.mounted;
    app.sidebar_locations.push_back(std::move(loc));
  }

  for (size_t i = 0; i < app.saved_searches.size(); ++i) {
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::SavedSearch;
    loc.label = app.saved_searches[i].query;
    loc.path = app.saved_searches[i].base_path;
    loc.icon_name = "system-search";
    loc.saved_search_idx = static_cast<int>(i);
    app.sidebar_locations.push_back(std::move(loc));
  }
}

// The ONLY place the sidebar's contribution to the content column is
// computed. Must use app.sidebar_w() (fold-aware), matching paint_inline_
// sidebar and paint_sidebar_flap, so a folded sidebar frees its width
// everywhere at once.

void sidebar_content_geometry(AppState& app, int& cx, int& cy, int& cw,
                              int& ch, int& banner_h) {
  const int w = app.width;
  const int h = app.height;
  int sidebar_w = app.sidebar_w();
  int info_panel_w = app.info_panel_open ? app.info_panel_width : 0;
  if (info_panel_w > 0) {
    int max_info = w - 300;
    if (max_info > 320) max_info = 320;
    if (max_info < 200) max_info = 200;
    if (info_panel_w > max_info) info_panel_w = max_info;
  }
  const int top_h = app.top_bar_height;
  const int tab_h = app.tabs.size() > 1 ? app.tab_bar_height : 0;
  const int status_h = app.status_bar_height;
  int selector_h =
      (app.select_dir_mode || app.select_file_mode) ? app.select_bar_h : 0;
  banner_h = (app.search_active || app.recursive_search_active ||
              app.r_search_active || app.r_recursive_search_active)
                 ? 28
                 : 0;
  cx = sidebar_w;
  cw = w - sidebar_w - info_panel_w;
  cy = top_h + tab_h + banner_h;
  ch = h - top_h - tab_h - banner_h - status_h - selector_h;
}


void update_sidebar_fold(AppState& app, int width) {
  bool prev_folded = app.sidebar_folded;
  app.sidebar_folded =
      !app.split_view &&
      width < std::max(AppState::kSidebarFoldBreakpoint,
                       app.top_bar_min_width());
  // Fold state never changes -> reveal state never lingers on.
  if (app.sidebar_folded != prev_folded) app.sidebar_folded_revealed = false;
  if (!app.sidebar_folded) app.sidebar_folded_revealed = false;
}


void paint_inline_sidebar(AppState& app, cairo_t* cr, int h, int status_h,
                          int view_h) {
  int sidebar_w = app.sidebar_w();
  int sidebar_h = h - status_h;
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, sidebar_w, sidebar_h);
  cairo_clip(cr);
  double sidebar_alpha = app.sidebar_opacity_pct / 100.0;
  cairo_set_source_rgba(cr, app.surface_r * 2, app.surface_g * 2,
                        app.surface_b * 2, sidebar_alpha);
  cairo_rectangle(cr, 0, 0, sidebar_w, sidebar_h);
  cairo_fill(cr);
  draw_sidebar(app, cr, sidebar_w, 0, view_h);
  cairo_restore(cr);

  // Sidebar separator (and resize handle)
  if (sidebar_w > 0) {
    int sep_h = h - status_h;
    if (app.sidebar_hover_resize) {
      cairo_set_source_rgba(cr, 0.4, 0.6, 1.0, 0.6);
      cairo_rectangle(cr, sidebar_w - 1, 0, 3, sep_h);
    } else {
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b,
                            0.3);
      cairo_rectangle(cr, sidebar_w, 0, 1, sep_h);
    }
    cairo_fill(cr);
  }
}

void paint_sidebar_flap(AppState& app, cairo_t* cr, int w, int h, int view_h,
                        int status_h) {
  // Adaptive sidebar fold flap: the sidebar is a temporary overlay covering
  // only the content column (below top/tab/banner bars), leaving the toolbars
  // fully interactive while a strip of content peeks out to the right.
  if (!(app.sidebar_folded && app.sidebar_folded_revealed)) return;
  int o_w = app.effective_sidebar_width();
  int flap_top = app.content_top_y();
  int flap_bottom = h - status_h;
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.15);
  cairo_rectangle(cr, o_w, flap_top, w - o_w, flap_bottom - flap_top);
  cairo_fill(cr);
  cairo_save(cr);
  cairo_rectangle(cr, 0, flap_top, o_w, flap_bottom - flap_top);
  cairo_clip(cr);
  double flap_alpha = app.sidebar_opacity_pct / 100.0;
  cairo_set_source_rgba(cr, app.surface_r * 2, app.surface_g * 2,
                        app.surface_b * 2, flap_alpha);
  cairo_rectangle(cr, 0, flap_top, o_w, flap_bottom - flap_top);
  cairo_fill(cr);
  draw_sidebar(app, cr, o_w, flap_top, view_h);
  cairo_restore(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  cairo_rectangle(cr, o_w, flap_top, 1, flap_bottom - flap_top);
  cairo_fill(cr);
}


bool sidebar_toggle_hit(AppState& app, int x, int y) {
  // Resolved through the retained hit registry (rect stored during paint);
  // no geometry is re-derived here.
  if (!app.sidebar_folded) return false;
  const uint32_t hid = app.hit_main.query(x, y);
  return hid == hui::Hit::topbar(0, hui::Hit::kTopFoldToggle) ||
         hid == hui::Hit::topbar(1, hui::Hit::kTopFoldToggle);
}

void toggle_sidebar_flap(AppState& app) {
  app.sidebar_folded_revealed = !app.sidebar_folded_revealed;
  app.sidebar_hover_idx = -1;
}

void dismiss_sidebar_flap(AppState& app, int x, int y) {
  // The flap is a transient overlay: clicking anywhere outside the flap
  // closes it. The click is still processed normally (it acts on whatever
  // was clicked), matching the flap overlay behavior.
  if (!(app.sidebar_folded && app.sidebar_folded_revealed)) return;
  int o_w = app.effective_sidebar_width();
  bool on_toggle = sidebar_toggle_hit(app, x, y);
  bool in_flap = x < o_w && y >= app.content_top_y();
  if (!in_flap && !on_toggle) app.sidebar_folded_revealed = false;
}

bool begin_sidebar_resize(AppState& app, int x) {
  if (app.sidebar_w() <= 0) return false;
  int edge_x = app.sidebar_width;
  if (x < edge_x - 4 || x > edge_x + 4) return false;
  app.sidebar_dragging = true;
  app.sidebar_drag_start_x = x;
  app.sidebar_drag_start_width = app.sidebar_width;
  return true;
}

void drag_sidebar_resize(AppState& app, int x) {
  if (!app.sidebar_dragging) return;
  int delta = x - app.sidebar_drag_start_x;
  int new_width = std::clamp(app.sidebar_drag_start_width + delta, 120, 400);
  app.sidebar_width = new_width;
  app.sidebar_width_base =
      static_cast<int>(new_width * 100.0 / app.zoom_pct);
}

void end_sidebar_resize(AppState& app) { app.sidebar_dragging = false; }

/// Begin a scrollbar thumb drag (pointer pressed on the thumb).
/// Returns true when a drag actually started (caller should consume it).
bool begin_sidebar_scroll(AppState& app, int x, int y) {
  if (app.sidebar_scroll_max <= 0 || app.sidebar_scroll_thumb[3] <= 0)
    return false;
  int tx = app.sidebar_scroll_track[0], ty = app.sidebar_scroll_track[1];
  int tw = app.sidebar_scroll_track[2], th = app.sidebar_scroll_track[3];
  if (x < tx || x >= tx + tw || y < ty || y >= ty + th) return false;
  app.sidebar_scroll_dragging = true;
  app.sidebar_scroll_grab = y - app.sidebar_scroll_thumb[1];
  return true;
}

/// Continue a scrollbar thumb drag.
void drag_sidebar_scroll(AppState& app, int y) {
  if (!app.sidebar_scroll_dragging) return;
  int track_y = app.sidebar_scroll_track[1];
  int track_h = app.sidebar_scroll_track[3];
  int thumb_h = app.sidebar_scroll_thumb[3];
  int span = std::max(1, track_h - thumb_h);
  int pos = std::clamp(y - app.sidebar_scroll_grab - track_y, 0, span);
  app.sidebar_scroll_px =
      std::clamp(app.sidebar_scroll_max * pos / span, 0,
                 app.sidebar_scroll_max);
}

/// Release the scrollbar drag.
void end_sidebar_scroll(AppState& app) { app.sidebar_scroll_dragging = false; }

bool update_sidebar_resize_hover(AppState& app, int x) {
  if (app.sidebar_w() > 0) {
    int edge_x = app.sidebar_width;
    bool over = (x >= edge_x - 4 && x <= edge_x + 4);
    if (over != app.sidebar_hover_resize) {
      app.sidebar_hover_resize = over;
      return true;
    }
  } else if (app.sidebar_hover_resize) {
    app.sidebar_hover_resize = false;
    return true;
  }
  return false;
}

}  // namespace eh::file_browser
