// preview_popup.cpp
// Moved wholesale from features/nav.cpp (byte-identical bodies).


// Must come first: exposes struct statx through <sys/stat.h>.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "../trace.hpp"

#include "../app.hpp"

#include <chrono>

#include "app/file_browser/features/desktop_icon_parser.hpp"
#include "app/file_browser/features/dirprops.hpp"
#include "app/file_browser/features/query_match.hpp"
#include "app/file_browser/features/recursive_search_worker.hpp"
#include "app/file_browser/features/tab_history.hpp"
#include "app/file_browser/features/thumb_pool.hpp"
#include "app/file_browser/features/filetype.hpp"
#include "app/file_browser/features/nav.hpp"
#include "app/file_browser/ui/layout.hpp"



#include "app/file_browser/features/video_worker.hpp"
#include "app/file_browser/features/svg_preview.hpp"
#include "app/file_browser/features/pdf_preview.hpp"
#include "app/file_browser/features/epub_preview.hpp"
#include "app/file_browser/features/image_preview.hpp"
#include "app/file_browser/features/view_zoom.hpp"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mntent.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#if defined(EH_HAVE_IO_URING)
#include <liburing.h>
#endif

#include "services/udisks2/drive_filter.hpp"

#include <gio/gio.h>

#include "services/udisks2/udisks2_drive_service.hpp"

#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "platform/common/ns/namespaces.hpp"
#include "wayland/surface/layer_surface.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;


namespace eh::file_browser {
void preview_log(const char* fmt, ...);  // defined in draw.cpp
// ── hover preview helper ─────────────────────────────────────────

// Forward declarations for preview popup helpers (defined below)
static void destroy_preview_popup(AppState& app);
static bool create_preview_popup(AppState& app);
static void commit_preview_popup(AppState& app);
static void hover_anchor_point(AppState& app, int vi, int& mx, int& my);

// Aspect-fit the hover popup around a full-res frame. The media card draws
// the image over the whole body (pw × (ph − footer)), so this mirrors that
// exactly: frame aspect dictates body size, then the footer band is added.
// That keeps the drawn frame (contain, centered) filling the entire body
// with nothing cropped — a cover-fit would otherwise clip wide frames.
static void size_hover_popup_to_frame(int& popup_w, int& popup_h,
                                      int tw, int th) {
  int max_w = 700;
  int max_h = 800;
  const int footer = 26;     // filename/info band at the bottom
  double scale = std::min({static_cast<double>(max_w) / tw,
                          static_cast<double>(max_h - footer) / th, 1.0});
  popup_w = std::max(150, static_cast<int>(tw * scale));
  popup_h = std::max(150, static_cast<int>(th * scale + footer));
}

// Scale the popup *uniformly* so it always matches the image's aspect ratio
// once preview_scale / the screen forces a smaller fit — otherwise the full-
// bleed image would letterbox or crop.
static void fit_hover_popup_to_screen(AppState& app, int& popup_w, int& popup_h) {
  const double sc = std::clamp(app.preview_scale, 1.0, 10.0);
  popup_w = static_cast<int>(static_cast<double>(popup_w) * sc);
  popup_h = static_cast<int>(static_cast<double>(popup_h) * sc);
  const int max_w = std::max(120, app.width - 32);
  const int max_h = std::max(120, app.height - app.status_bar_height - 48);
  if (popup_w > max_w || popup_h > max_h) {
    const double s = std::min(static_cast<double>(max_w) / popup_w,
                              static_cast<double>(max_h) / popup_h);
    if (s < 1.0) {
      popup_w = static_cast<int>(static_cast<double>(popup_w) * s);
      popup_h = static_cast<int>(static_cast<double>(popup_h) * s);
    }
  }
  popup_w = std::max(120, popup_w);
  popup_h = std::max(120, popup_h);
}

// Position the hover popup centered on mouse X, above mouse Y, clamped on screen
static bool preview_dbg() {
  static const bool on = [] {
    const char* e = std::getenv("EH_PREVIEW_DEBUG");
    return e && *e && e[0] != '0';
  }();
  return on;
}

static void place_hover_popup(AppState& app, int mx, int my,
                              int popup_w, int popup_h) {
  int popup_x = mx - popup_w / 2;
  int popup_y = my - popup_h - 20; // above cursor with 20px gap

  int content_x = app.sidebar_w();
  int content_y = app.top_bar_height + app.tab_bar_height;
  if (popup_x < content_x + 10) popup_x = content_x + 10;
  if (popup_x + popup_w > app.width - 10)
    popup_x = app.width - popup_w - 10;
  if (popup_y < content_y + 10)          // above viewport → flip below
    popup_y = my + 20;
  if (popup_y + popup_h > app.height - app.status_bar_height - 10)
    popup_y = app.height - app.status_bar_height - popup_h - 10;

  app.preview_x = popup_x;
  app.preview_y = popup_y;
  app.preview_w = popup_w;
  app.preview_h = popup_h;
}

void resize_active_image_preview(AppState& app) {
  if (!app.preview_active || !app.preview_thumb) return;
  const int vi = app.preview_entry_idx;
  if (vi < 0 || vi >= static_cast<int>(app.cur_tab().visible_entries.size()))
    return;
  const int ri = app.cur_tab().visible_entries[vi];
  if (ri < 0 || ri >= static_cast<int>(app.cur_tab().entries.size())) return;
  if (app.cur_tab().entries[ri].type != FileType::Image) return;
  const int tw = cairo_image_surface_get_width(app.preview_thumb);
  const int th = cairo_image_surface_get_height(app.preview_thumb);
  if (tw <= 0 || th <= 0) return;
  int popup_w = 260, popup_h = 240;
  size_hover_popup_to_frame(popup_w, popup_h, tw, th);
  fit_hover_popup_to_screen(app, popup_w, popup_h);
  int mx = 0, my = 0;
  hover_anchor_point(app, vi, mx, my);
  place_hover_popup(app, mx, my, popup_w, popup_h);
  if (create_preview_popup(app))
    commit_preview_popup(app);
  app.pendingRedraw = true;
}

void reset_preview(AppState& app) {
  // Clean up popup surface if it exists
  destroy_preview_popup(app);
  app.preview_active = false;
  app.preview_mode = AppState::PreviewMode::None;
  app.preview_entry_idx = -1;
  app.preview_path.clear();
  app.preview_text.clear();
  app.preview_hover_start_ns = 0;
  app.preview_req_path.clear();
  app.preview_req_px = 0;
  if (app.preview_thumb) {
    cairo_surface_destroy(app.preview_thumb);
    app.preview_thumb = nullptr;
  }
}

// The entry an overlay (hover preview / tooltip) should currently describe:
// the hovered entry when the mouse is over one, else the keyboard-selected
// entry. A left-click selects too, but a mouse-selected item must NOT count as
// a hover once the cursor leaves it, so the selected fallback only engages
// when the selection was made by the keyboard (selected_by_kbd). Only
// List/Grid/Compact index both hover and selection into visible_entries —
// Tree and Computer keep their own arrays, so the keyboard fallback is
// disabled there (hover still works via hover_idx).
static int overlay_target_idx(const AppState& app) {
  if (app.cur_tab().hover_idx >= 0) return app.cur_tab().hover_idx;
  auto vm = app.cur_tab().view_mode;
  if ((vm == ViewMode::List || vm == ViewMode::Grid || vm == ViewMode::Compact) &&
      app.cur_tab().selected_by_kbd)
    return app.cur_tab().selected_idx;
  return -1;
}

// Types that get a live hover preview (mirrors the arming conditions in
// pointer.cpp — keep in sync when a type is added or removed).
static bool is_previewable_entry(const FileEntry& e) {
  return e.type == FileType::Image || e.type == FileType::Video ||
         e.type == FileType::Audio || e.type == FileType::Text ||
         e.type == FileType::Document || e.type == FileType::Code ||
         e.type == FileType::Archive || e.type == FileType::Web ||
         e.type == FileType::Font || e.type == FileType::Executable ||
         e.type == FileType::Markdown;
}

// Anchor point for popup placement: the cursor when mouse-driven, otherwise
// the keyed entry's rect center (so keyboard nav floats the overlay beside
// the selection instead of the last cursor position).
static void hover_anchor_point(AppState& app, int vi, int& mx, int& my) {
  if (app.cur_tab().hover_idx >= 0) {
    mx = static_cast<int>(app.pointerX);
    my = static_cast<int>(app.pointerY);
    return;
  }
  double zf = app.zoom_pct / 100.0;
  PaneViewRect pr = pane_view_rect_for_index(app, app.active_pane);
  if (app.cur_tab().view_mode == ViewMode::Grid) {
    GridLayout gl = compute_grid_layout(std::max(0, pr.w), zf);
    if (gl.cols <= 0) { mx = pr.x; my = pr.y; return; }
    int col = vi % gl.cols;
    int row = vi / gl.cols;
    mx = pr.x + gl.grid_offset_x + col * (gl.cell_w + gl.col_gap) + gl.cell_w / 2;
    my = pr.y + gl.row_gap - app.cur_tab().scroll_px +
         row * gl.row_h + gl.item_h / 2;
    return;
  }
  ListLayout ll = compute_list_layout(zf);
  int headers = 0;
  if (app.cur_tab().group_by_type) {
    int prev = -1;
    for (int i = 0; i <= vi; ++i) {
      int r = app.cur_tab().visible_entries[i];
      if (r < 0 || r >= static_cast<int>(app.cur_tab().entries.size())) continue;
      int t = static_cast<int>(app.cur_tab().entries[r].type);
      if (t != prev) { ++headers; prev = t; }
    }
  }
  int header_h = static_cast<int>(ll.entry_h * 0.55);
  int top = pr.y + ll.col_header_h - app.cur_tab().scroll_px +
            vi * ll.entry_h + headers * header_h;
  mx = pr.x + ll.entry_h;
  my = top + ll.entry_h / 2;
}

void check_hover_preview(AppState& app) {
  // Don't touch space preview
  if (app.preview_mode == AppState::PreviewMode::Space) return;

  // A right-click menu must never sit under (or re-arm) a hover preview.
  if (app.context_menu_open) {
    if (app.preview_entry_idx >= 0) reset_preview(app);
    return;
  }

  // Guard: if index is stale (entries changed), reset
  int n_visible = static_cast<int>(app.cur_tab().visible_entries.size());
  if (app.preview_entry_idx >= n_visible) {
    reset_preview(app);
    return;
  }

  int target = overlay_target_idx(app);

  // If the entry is no longer hovered, cancel
  if (app.preview_entry_idx >= 0 && app.preview_entry_idx != target) {
    reset_preview(app);
    return;
  }

  // Nothing armed: self-arm for a keyboard-selected previewable entry
  // (mouse-driven arming lives in pointer.cpp). The 400ms timer below then
  // activates it on a later frame, same as hover.
  if (app.preview_entry_idx < 0 && !app.preview_active) {
    if (target < 0 || target >= n_visible) return;
    int tri = app.cur_tab().visible_entries[target];
    if (tri < 0 || tri >= static_cast<int>(app.cur_tab().entries.size())) return;
    const FileEntry& tentry = app.cur_tab().entries[tri];
    if (!is_previewable_entry(tentry)) return; // dirs get the tooltip instead
    app.preview_entry_idx = target;
    app.preview_path = tentry.path;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    app.preview_hover_start_ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                                  static_cast<uint64_t>(ts.tv_nsec);
    return;
  }

  // Timer: activate after 400ms
  if (!app.preview_active && app.preview_entry_idx >= 0) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                   static_cast<uint64_t>(ts.tv_nsec);
    if (now - app.preview_hover_start_ns > 400000000) { // 400ms
      auto hover_t0 = std::chrono::steady_clock::now();
      struct HoverTimer {
        std::chrono::steady_clock::time_point t0;
        ~HoverTimer() {
          double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
          if (ms >= 50.0 && trace::enabled().load(std::memory_order_relaxed))
            trace::log("HOVER SLOW %.1f ms", ms);
        }
      } hover_timer{hover_t0};
      int vi = app.preview_entry_idx;
      if (vi >= 0 && vi < static_cast<int>(app.cur_tab().visible_entries.size())) {
        int ri = app.cur_tab().visible_entries[vi];
        if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
          const auto& entry = app.cur_tab().entries[ri];

          int mx = 0, my = 0;
          hover_anchor_point(app, vi, mx, my);

          // Iron rule: never decode on the paint thread. Open the popup at
          // default size now; the background pool's result upgrades it when
          // it lands (see retry section below). Videos already use their own
          // async worker.
          if (entry.type == FileType::Image) {
            app.preview_req_px = 500;
            app.preview_req_path = entry.path;
            if (preview_dbg()) fprintf(stderr, "[preview] open req='%s' px=500\n", entry.path.c_str());
            thumb_pool_enqueue(app, entry.path, app.preview_req_px);
          } else if (entry.type == FileType::Video) {
            video_worker().enqueue_preview(entry.path, kVideoPreviewFrameMaxPx);
          } else if (entry.type == FileType::Audio) {
            app.preview_req_px = 256;
            app.preview_req_path = entry.path;
            thumb_pool_enqueue(app, entry.path, app.preview_req_px);
          } else if (entry.type == FileType::Document && is_pdf_extension(entry.path)) {
            app.preview_req_px = 256;
            app.preview_req_path = entry.path;
            thumb_pool_enqueue(app, entry.path, app.preview_req_px);
          } else if (entry.type == FileType::Document && is_epub_extension(entry.path)) {
            app.preview_req_px = 256;
            app.preview_req_path = entry.path;
            thumb_pool_enqueue(app, entry.path, app.preview_req_px);
          }

          // Read text file content for preview (not for binary Document types like PDF/EPUB)
          if (entry.type == FileType::Text || entry.type == FileType::Markdown ||
              entry.type == FileType::Code) {
            FILE* f = fopen(entry.path.c_str(), "r");
            if (f) {
              char buf[513];
              size_t n = fread(buf, 1, 512, f);
              fclose(f);
              buf[n] = '\0';
              app.preview_text.assign(buf, n);
            }
          }

          // Compute popup size — images get aspect-ratio-sized popup
          int popup_w = 260;
          int popup_h = 240;
          if (entry.type == FileType::Image && app.preview_thumb) {
            int tw = cairo_image_surface_get_width(app.preview_thumb);
            int th = cairo_image_surface_get_height(app.preview_thumb);
            if (tw > 0 && th > 0) {
              size_hover_popup_to_frame(popup_w, popup_h, tw, th);
              fit_hover_popup_to_screen(app, popup_w, popup_h);
            }
          }

          place_hover_popup(app, mx, my, popup_w, popup_h);
          app.preview_path = entry.path;

          // Create + draw + commit the subsurface popup
          if (create_preview_popup(app))
            commit_preview_popup(app);

          app.preview_active = true;
          app.preview_mode = AppState::PreviewMode::Hover;
          app.pendingRedraw = true;
        }
      }
    }
  }

  // Re-try thumbnail for async decodes (video, etc.) while preview is active
  if (app.preview_active && app.preview_entry_idx >= 0) {
    int vi = app.preview_entry_idx;
    if (vi >= 0 && vi < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int ri = app.cur_tab().visible_entries[vi];
      if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
        const auto& entry = app.cur_tab().entries[ri];
        if (entry.type == FileType::Video) {
          // Poll for the full-res frame extraction
          VideoThumbResult res;
          while (video_worker().poll_preview(res)) {
            if (res.path != app.preview_path) {
              // Stale result from an earlier hover
              if (res.surface) cairo_surface_destroy(res.surface);
              continue;
            }
            if (res.surface) {
              // Upgrade: swap in the full-res frame and resize the popup
              // around it, like images
              if (app.preview_thumb)
                cairo_surface_destroy(app.preview_thumb);
              app.preview_thumb = res.surface; // take ownership
              int tw = cairo_image_surface_get_width(app.preview_thumb);
              int th = cairo_image_surface_get_height(app.preview_thumb);
              if (tw > 0 && th > 0) {
                int popup_w = 260;
                int popup_h = 240;
                size_hover_popup_to_frame(popup_w, popup_h, tw, th);
                fit_hover_popup_to_screen(app, popup_w, popup_h);
                int ax = 0, ay = 0;
                hover_anchor_point(app, vi, ax, ay);
                place_hover_popup(app, ax, ay, popup_w, popup_h);
                if (create_preview_popup(app))
                  commit_preview_popup(app);
              }
              app.pendingRedraw = true;
            } else if (!app.preview_thumb) {
              // Extraction failed — fall back to the low-res cached thumb
              cairo_surface_t* thumb = get_thumbnail(app, entry.path, 256);
              if (thumb) {
                cairo_surface_reference(thumb);
                app.preview_thumb = thumb;
                app.pendingRedraw = true;
              }
            }
            break;
          }
        } else if (!app.preview_req_path.empty() &&
                   entry.path == app.preview_req_path) {
          // Background decode requested at hover start — upgrade as soon as
          // the pool's result lands in the thumbnail cache. The cache keeps
          // the largest decode (grid lows are replaced by the hi-res preview
          // request), so accept whatever appears; keep waiting while the
          // final-size decode hasn't landed yet so a genuinely small source
          // image (native < req) still previews at its true size.
          auto it = app.thumb_cache.find(entry.path);
          if (it != app.thumb_cache.end()) {
            cairo_surface_t* s2 = it->second;
            int tw = cairo_image_surface_get_width(s2);
            int th = cairo_image_surface_get_height(s2);
            bool final_size =
                std::max(tw, th) >= (app.preview_req_px * 3) / 4;
            bool larger = true;
            if (app.preview_thumb) {
              int ow = cairo_image_surface_get_width(app.preview_thumb);
              int oh = cairo_image_surface_get_height(app.preview_thumb);
              larger = std::max(tw, th) > std::max(ow, oh);
            }
            if (larger) {
              if (preview_dbg()) fprintf(stderr, "[preview] accept '%s' %dx%d%s\n", entry.path.c_str(), tw, th, final_size ? " (final)" : "");
              cairo_surface_reference(s2);
              if (app.preview_thumb) cairo_surface_destroy(app.preview_thumb);
              app.preview_thumb = s2;
              if (entry.type == FileType::Image && tw > 0 && th > 0) {
                int popup_w = 260;
                int popup_h = 240;
                size_hover_popup_to_frame(popup_w, popup_h, tw, th);
                fit_hover_popup_to_screen(app, popup_w, popup_h);
                int ax = 0, ay = 0;
                hover_anchor_point(app, vi, ax, ay);
                place_hover_popup(app, ax, ay, popup_w, popup_h);
                if (create_preview_popup(app))
                  commit_preview_popup(app);
              }
              app.pendingRedraw = true;
              if (final_size) app.preview_req_path.clear();
            }
          }
        }
      }
    }
  }
}

// ── Rich tooltip popup subsurface helpers ─────────────────────

static void destroy_tooltip_popup(AppState& app) {
  app.tooltipPopupBuf.destroy();
  if (app.tooltipPopupSub) {
    wl_subsurface_destroy(app.tooltipPopupSub);
    app.tooltipPopupSub = nullptr;
  }
  if (app.tooltipPopupSurface) {
    wl_surface_attach(app.tooltipPopupSurface, nullptr, 0, 0);
    wl_surface_commit(app.tooltipPopupSurface);
    wl_surface_destroy(app.tooltipPopupSurface);
    app.tooltipPopupSurface = nullptr;
  }
}

void hide_tooltip(AppState& app) {
  if (!app.tooltip_active && app.tooltip_path.empty() &&
      !app.tooltipPopupSurface)
    return;
  destroy_tooltip_popup(app);
  app.tooltip_active = false;
  app.tooltip_path.clear();
  app.tooltip_show_ms = 0;
}

static bool create_tooltip_popup(AppState& app) {
  destroy_tooltip_popup(app);

  if (app.tooltip_w < 1 || app.tooltip_h < 1) return false;

  wl_surface* surf = wl_compositor_create_surface(app.wl.compositor());
  if (!surf) return false;

  wl_subsurface* sub = wl_subcompositor_get_subsurface(app.wl.subcompositor(), surf, app.surface);
  if (!sub) {
    wl_surface_destroy(surf);
    return false;
  }

  int shadow_pad = 6;
  wl_subsurface_set_position(sub, app.tooltip_x - shadow_pad, app.tooltip_y - shadow_pad);
  wl_subsurface_place_above(sub, app.surface);

  app.tooltipPopupSurface = surf;
  app.tooltipPopupSub = sub;

  wl_surface_commit(app.tooltipPopupSurface);
  return true;
}

// Draw content is provided by draw.cpp; commit mirrors the preview popup.
void draw_tooltip_card(AppState& app, cairo_t* cr); // defined in draw_popups.cpp

static void commit_tooltip_popup(AppState& app) {
  if (!app.tooltipPopupSurface || !app.tooltipPopupSub) return;

  int pw = app.tooltip_w;
  int ph = app.tooltip_h;
  if (pw < 1 || ph < 1) return;

  int shadow_pad = 6;
  int buf_w = pw + shadow_pad * 2;
  int buf_h = ph + shadow_pad * 2;

  app.tooltipPopupBuf.ensure(app.shm, eh::shell::kPopupNamespace, buf_w, buf_h);
  cairo_t* cr = app.tooltipPopupBuf.cairo();
  if (!cr) return;

  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_restore(cr);

  cairo_save(cr);
  cairo_translate(cr, -app.tooltip_x + shadow_pad, -app.tooltip_y + shadow_pad);
  draw_tooltip_card(app, cr);
  cairo_restore(cr);

  cairo_surface_flush(app.tooltipPopupBuf.cairo_surface());

  wl_surface_attach(app.tooltipPopupSurface, app.tooltipPopupBuf.wl(), 0, 0);
  wl_surface_damage_buffer(app.tooltipPopupSurface, 0, 0, buf_w, buf_h);
  app.tooltipPopupBuf.mark_busy();
  wl_surface_commit(app.tooltipPopupSurface);
  wl_display_flush(app.wl.display());
}

void check_hover_tooltip(AppState& app) {
  if (app.preview_mode == AppState::PreviewMode::Space) return;
  if (app.context_menu_open || app.preview_active) return;

  long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  int target = overlay_target_idx(app);
  int n_visible = static_cast<int>(app.cur_tab().visible_entries.size());

  // Nothing armed: self-arm for a keyboard-selected directory (mouse-driven
  // arming lives in pointer.cpp). The delay gate below then shows it later.
  if (app.tooltip_path.empty() && !app.tooltip_active) {
    if (target < 0 || target >= n_visible) return;
    int tri = app.cur_tab().visible_entries[target];
    if (tri < 0 || tri >= static_cast<int>(app.cur_tab().entries.size())) return;
    const FileEntry& tentry = app.cur_tab().entries[tri];
    if (is_previewable_entry(tentry) || !tentry.is_dir) return; // preview types
    app.tooltip_path = tentry.path;
    app.tooltip_active = false;
    app.tooltip_show_ms = now_ms + 200;
    return;
  }
  // Still hovering/selecting the armed entry? Validate even while shown so
  // keyboard navigation dismisses a stale tooltip (no mouse event to do it).
  int vi = target;
  if (vi < 0 || vi >= n_visible) {
    hide_tooltip(app);
    return;
  }
  int ri = app.cur_tab().visible_entries[vi];
  if (ri < 0 || ri >= static_cast<int>(app.cur_tab().entries.size()) ||
      app.cur_tab().entries[ri].path != app.tooltip_path) {
    hide_tooltip(app);
    return;
  }
  if (app.tooltip_active || app.tooltip_path.empty()) return;
  if (now_ms < app.tooltip_show_ms) return;

  const FileEntry& entry = app.cur_tab().entries[ri];

  // Metadata rows
  app.tooltip_title = entry.name;
  auto tip_t0 = std::chrono::steady_clock::now();
  struct TipTimer {
    std::chrono::steady_clock::time_point t0;
    ~TipTimer() {
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
      if (ms >= 50.0 && trace::enabled().load(std::memory_order_relaxed))
        trace::log("TOOLTIP SLOW %.1f ms", ms);
    }
  } tip_timer{tip_t0};
  app.tooltip_rows.clear();
  auto fmt_size = [](uint64_t bytes) -> std::string {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024ull * 1024 * 1024)
      return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
  };
  const char* type_names[] = {"Folder", "Image", "Audio", "Video", "Text",
                              "Markdown", "Code", "Document", "Font",
                              "Archive", "Executable", "Web", "File"};
  {
    int ti = static_cast<int>(entry.type);
    std::string row = (ti >= 0 && ti < 13) ? type_names[ti] : "File";
    if (!entry.is_dir) row += "  \u00B7  " + fmt_size(entry.size);
    app.tooltip_rows.push_back(row);
  }
  if (entry.is_dir) {
    // Item count via bounded readdir
    long count = 0;
    if (DIR* d = opendir(entry.path.c_str())) {
      while (struct dirent* de = readdir(d)) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        if (++count > 9999) { count = 9999; break; }
      }
      closedir(d);
    }
    app.tooltip_rows.push_back(count >= 9999 ? "9999+ items"
                                             : std::to_string(count) + " items");
  }
  {
    char mb[64]{};
    struct tm tm_buf{};
    time_t mt = static_cast<time_t>(entry.modified_sec);
    if (localtime_r(&mt, &tm_buf))
      strftime(mb, sizeof(mb), "%Y-%m-%d %H:%M", &tm_buf);
    app.tooltip_rows.push_back(std::string("Modified ") + mb);
  }
  if (!entry.owner.empty())
    app.tooltip_rows.push_back(entry.owner + ":" + entry.group + "  " +
                               format_mode(entry.mode));

  // Size: title line + rows. Widen beyond the old fixed 250px so long file
  // names read in full — measure title (bold 12) and rows (11) against a
  // scratch cairo context (font config is warm from startup).
  int pad = 12;
  int max_text_w = 0;
  {
    cairo_surface_t* meas_surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t* mcr = cairo_create(meas_surf);
    cairo_select_font_face(mcr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(mcr, 12);
    cairo_text_extents_t mte;
    cairo_text_extents(mcr, app.tooltip_title.c_str(), &mte);
    max_text_w = static_cast<int>(mte.width);
    cairo_select_font_face(mcr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(mcr, 11);
    for (const auto& row : app.tooltip_rows) {
      cairo_text_extents(mcr, row.c_str(), &mte);
      max_text_w = std::max(max_text_w, static_cast<int>(mte.width));
    }
    cairo_destroy(mcr);
    cairo_surface_destroy(meas_surf);
  }
  int card_w = std::clamp(max_text_w + pad * 2 + 8, 250, 600);
  int row_h = 20;
  int title_h = 24;
  int card_h = pad * 2 + title_h +
               static_cast<int>(app.tooltip_rows.size()) * row_h;
  app.tooltip_w = card_w;
  app.tooltip_h = card_h;

  // Position below-right of the anchor (cursor or selected entry), clamped
  // to viewport
  {
    int mx = 0, my = 0;
    hover_anchor_point(app, vi, mx, my);
    int tx = mx + 16;
    int ty = my + 18;
    int content_x = app.sidebar_w();
    int content_y = app.top_bar_height + app.tab_bar_height;
    if (tx + card_w > app.width - 10) tx = app.width - card_w - 10;
    if (tx < content_x + 10) tx = content_x + 10;
    if (ty + card_h > app.height - app.status_bar_height - 10)
      ty = my - card_h - 14;
    if (ty < content_y + 10) ty = content_y + 10;
    app.tooltip_x = tx;
    app.tooltip_y = ty;
  }

  if (create_tooltip_popup(app)) commit_tooltip_popup(app);
  app.tooltip_active = true;
  app.pendingRedraw = true;
}

// ── Preview popup subsurface helpers ──────────────────────────

static void destroy_preview_popup(AppState& app) {
  app.previewPopupBuf.destroy();
  if (app.previewPopupSub) {
    wl_subsurface_destroy(app.previewPopupSub);
    app.previewPopupSub = nullptr;
  }
  if (app.previewPopupSurface) {
    wl_surface_attach(app.previewPopupSurface, nullptr, 0, 0);
    wl_surface_commit(app.previewPopupSurface);
    wl_surface_destroy(app.previewPopupSurface);
    app.previewPopupSurface = nullptr;
  }
}

static bool create_preview_popup(AppState& app) {
  destroy_preview_popup(app);

  if (app.preview_w < 1 || app.preview_h < 1) return false;

  wl_surface* surf = wl_compositor_create_surface(app.wl.compositor());
  if (!surf) return false;

  wl_subsurface* sub = wl_subcompositor_get_subsurface(app.wl.subcompositor(), surf, app.surface);
  if (!sub) {
    wl_surface_destroy(surf);
    return false;
  }

  int shadow_pad = 6;
  wl_subsurface_set_position(sub, app.preview_x - shadow_pad, app.preview_y - shadow_pad);
  wl_subsurface_place_above(sub, app.surface);

  app.previewPopupSurface = surf;
  app.previewPopupSub = sub;

  wl_surface_commit(app.previewPopupSurface);
  return true;
}

static void commit_preview_popup(AppState& app) {
  if (!app.previewPopupSurface || !app.previewPopupSub) return;

  int pw = app.preview_w;
  int ph = app.preview_h;
  if (pw < 1 || ph < 1) return;

  int shadow_pad = 6;
  int buf_w = pw + shadow_pad * 2;
  int buf_h = ph + shadow_pad * 2;

  app.previewPopupBuf.ensure(app.shm, eh::shell::kPopupNamespace, buf_w, buf_h);
  cairo_t* cr = app.previewPopupBuf.cairo();
  if (!cr) return;

  // Clear to transparent
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_restore(cr);

  // Shift so the popup draws at (shadow_pad, shadow_pad) in the buffer,
  // leaving room for the drop shadow to extend right/down
  cairo_save(cr);
  cairo_translate(cr, -app.preview_x + shadow_pad, -app.preview_y + shadow_pad);
  draw_hover_preview(app, cr);
  cairo_restore(cr);

  cairo_surface_flush(app.previewPopupBuf.cairo_surface());

  wl_surface_attach(app.previewPopupSurface, app.previewPopupBuf.wl(), 0, 0);
  wl_surface_damage_buffer(app.previewPopupSurface, 0, 0, buf_w, buf_h);
  app.previewPopupBuf.mark_busy();
  wl_surface_commit(app.previewPopupSurface);
  wl_display_flush(app.wl.display());
}

// ── Space/Enter preview ─────────────────────────────────────────

void activate_space_preview(AppState& app) {
  int si = app.cur_tab().selected_idx;
  if (si < 0 || si >= static_cast<int>(app.cur_tab().visible_entries.size())) return;
  int ri = app.cur_tab().visible_entries[si];
  if (ri < 0 || ri >= static_cast<int>(app.cur_tab().entries.size())) return;
  const auto& entry = app.cur_tab().entries[ri];

  if (entry.is_dir) return;

  reset_preview(app);

  int thumb_px = 512;
  bool has_thumb = false;
  if (entry.type == FileType::Image) {
    if (is_svg_extension(entry.path))
      app.preview_thumb = load_svg_thumbnail(entry.path, thumb_px);
    else
      app.preview_thumb = load_image_thumbnail(entry.path, thumb_px);
    has_thumb = app.preview_thumb != nullptr;
    if (!has_thumb)
      preview_log("space_preview: IMAGE thumb FAIL path=%s", entry.path.c_str());
  } else if (entry.type == FileType::Video || entry.type == FileType::Audio) {
    app.preview_thumb = get_thumbnail(app, entry.path, thumb_px);
    has_thumb = app.preview_thumb != nullptr;
    if (!has_thumb)
      preview_log("space_preview: VIDEO/AUDIO thumb FAIL path=%s", entry.path.c_str());
  } else if (entry.type == FileType::Document) {
    if (is_pdf_extension(entry.path)) {
      app.preview_thumb = load_pdf_thumbnail(entry.path, thumb_px);
      has_thumb = app.preview_thumb != nullptr;
      if (!has_thumb)
        preview_log("space_preview: PDF thumb FAIL path=%s", entry.path.c_str());
    } else if (is_epub_extension(entry.path)) {
      app.preview_thumb = load_epub_thumbnail(entry.path, thumb_px);
      has_thumb = app.preview_thumb != nullptr;
      if (!has_thumb)
        preview_log("space_preview: EPUB thumb FAIL path=%s", entry.path.c_str());
    }
  }

  if (has_thumb)
    cairo_surface_reference(app.preview_thumb);

  if (entry.type == FileType::Text || entry.type == FileType::Markdown ||
      entry.type == FileType::Code) {
    FILE* f = fopen(entry.path.c_str(), "r");
    if (f) {
      char buf[2049];
      size_t n = fread(buf, 1, 2048, f);
      fclose(f);
      buf[n] = '\0';
      app.preview_text.assign(buf, n);
    }
  }

  // Popup sizing: fill proportionally within the viewport, centered
  int viewport_w = app.width;
  int viewport_h = app.height - app.status_bar_height;

  int max_w = std::clamp(viewport_w - 80, 300, 1000);
  int max_h = std::clamp(viewport_h - 80, 200, 900);
  int popup_w = max_w;
  int popup_h = max_h;

  if (has_thumb && app.preview_thumb) {
    int tw = cairo_image_surface_get_width(app.preview_thumb);
    int th = cairo_image_surface_get_height(app.preview_thumb);
    if (tw > 0 && th > 0) {
      int margin = 12;
      int bottom_h = 50;
      double scale = std::min(static_cast<double>(max_w - margin * 2) / tw,
                              static_cast<double>(max_h - margin * 2 - bottom_h) / th);
      popup_w = static_cast<int>(tw * scale + margin * 2);
      popup_h = static_cast<int>(th * scale + margin * 2 + bottom_h);
    }
  }

  int popup_x = (viewport_w - popup_w) / 2;
  int popup_y = (viewport_h - popup_h) / 2;
  if (popup_x < 10) popup_x = 10;
  if (popup_x + popup_w > viewport_w - 10) popup_x = viewport_w - popup_w - 10;
  if (popup_y < app.top_bar_height + app.tab_bar_height + 10)
    popup_y = app.top_bar_height + app.tab_bar_height + 10;
  if (popup_y + popup_h > viewport_h - 10)
    popup_y = viewport_h - popup_h - 10;

  app.preview_entry_idx = si;
  app.preview_path = entry.path;
  app.preview_x = popup_x;
  app.preview_y = popup_y;
  app.preview_w = popup_w;
  app.preview_h = popup_h;

  // Create + draw + commit the subsurface popup
  if (create_preview_popup(app))
    commit_preview_popup(app);

  app.preview_active = true;
  app.preview_mode = AppState::PreviewMode::Space;
}

void toggle_space_preview(AppState& app) {
  if (app.preview_mode == AppState::PreviewMode::Space) {
    reset_preview(app);
  } else {
    activate_space_preview(app);
  }
}

// ── lazy thumbnail processing + async search polling ────────────

bool process_pending_thumbnails(AppState& app) {
  // Drain completed async video thumbnails into the cache
  drain_video_thumbnails(app);

  // Poll recursive search results (both local and home-wide)
  bool search_is_active = (app.active_pane ? app.r_search_active : app.search_active) || (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active);
  if (search_is_active && !(app.active_pane ? app.r_search_query : app.search_query).empty()) {
    bool got_any = false;
    SearchResult sr;
    // Cache of per-directory .hidden sets for the current result batch
    static std::unordered_map<std::string, std::unordered_set<std::string>> sr_hidden;
    sr_hidden.clear();
    while (recursive_search_worker().poll(sr)) {
      got_any = true;
      FileEntry entry;
      std::string filename = sr.path;
      auto slash = filename.rfind('/');
      entry.name = (slash != std::string::npos) ? filename.substr(slash + 1) : filename;
      entry.path = sr.path;
      entry.is_dir = sr.is_dir;
      entry.size = 0;
      if (!sr.is_dir) {
        entry.mime_type = mime_by_ext(entry.name);
      }
      entry.type = detect_file_type(entry.name, sr.is_dir, entry.mime_type);
      entry.is_hidden = is_hidden_file(entry.name);

      // Honor the parent directory's .hidden file
      std::string parent = (slash != std::string::npos) ? filename.substr(0, slash)
                                                        : std::string();
      if (!parent.empty()) {
        auto it = sr_hidden.find(parent);
        if (it == sr_hidden.end())
          it = sr_hidden.emplace(parent, read_hidden_file(parent)).first;
        if (it->second.count(entry.name) > 0) entry.is_hidden = true;
      }
      if (!app.show_hidden && entry.is_hidden) continue;

      // XDG user-directory icons from system theme
      if (entry.is_dir) {
        static const std::string xdg_home = home_dir();
        if      (entry.path == xdg_home + "/Desktop")    entry.icon_name = "user-desktop";
        else if (entry.path == xdg_home + "/Documents")  entry.icon_name = "folder-documents";
        else if (entry.path == xdg_home + "/Downloads")  entry.icon_name = "folder-download";
        else if (entry.path == xdg_home + "/Music")      entry.icon_name = "folder-music";
        else if (entry.path == xdg_home + "/Pictures")   entry.icon_name = "folder-pictures";
        else if (entry.path == xdg_home + "/Videos")     entry.icon_name = "folder-videos";
        else if (entry.path == xdg_home + "/Public")     entry.icon_name = "folder-publicshare";
        else if (entry.path == xdg_home + "/Templates")  entry.icon_name = "folder-templates";
      }

      // Parse .desktop file icon
      if (!entry.is_dir) {
        auto dot = entry.name.rfind('.');
        if (dot != std::string::npos) {
          std::string ext = entry.name.substr(dot + 1);
          for (auto& c : ext) c = std::tolower(c);
          if (ext == "desktop") {
            auto icon = parse_desktop_icon(entry.path);
            if (!icon.empty()) entry.icon_name = std::move(icon);
          }
        }
      }

      // Derive Freedesktop icon name from MIME type (icon themes use `media-subtype` format)
      if (entry.icon_name.empty() && !entry.mime_type.empty()) {
        std::string icon = entry.mime_type;
        for (auto& c : icon) if (c == '/') c = '-';
        entry.icon_name = std::move(icon);
      }

      // Populate size and modification time from stat for filter support
      struct stat st;
      if (::stat(entry.path.c_str(), &st) == 0) {
        entry.size = static_cast<uint64_t>(st.st_size);
        entry.modified_sec = st.st_mtime;
      }

      app.cur_tab().entries.push_back(std::move(entry));
    }
    if (got_any) {
      app.cur_tab().visible_entries.clear();
      ++app.listing_epoch;  // scroll-delta content reuse invalid
      bool has_filters = (app.active_pane ? app.r_filter_type_idx : app.filter_type_idx) > 0 || (app.active_pane ? app.r_filter_size_idx : app.filter_size_idx) > 0 || (app.active_pane ? app.r_filter_date_idx : app.filter_date_idx) > 0;
      for (int i = 0; i < static_cast<int>(app.cur_tab().entries.size()); ++i) {
        if (!has_filters || matches_filter(app, app.cur_tab().entries[i]))
          app.cur_tab().visible_entries.push_back(i);
      }
      app.cur_tab().selected_idx = 0;
      app.cur_tab().scroll_px = 0;
      app.pendingRedraw = true;
    }
  }

  // Legacy visible-pending queue: now just forwards to the background
  // thumbnail pool (decoding never happens on this thread).
  for (int i = 0; i < AppState::kThumbDecodesPerLoop; ++i) {
    if (app.thumb_pending_queue.empty()) break;

    auto pending = app.thumb_pending_queue.back();
    app.thumb_pending_queue.pop_back();

    int vi = pending.visible_idx;
    if (vi < 0 || vi >= static_cast<int>(app.cur_tab().visible_entries.size()))
      continue;

    int real_idx = app.cur_tab().visible_entries[vi];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      continue;

    auto& entry = app.cur_tab().entries[real_idx];
    // Skip types that don't support thumbnails
    if (entry.type != FileType::Image && entry.type != FileType::Video) {
      if (entry.type != FileType::Document) continue;
      if (!is_pdf_extension(entry.path) && !is_epub_extension(entry.path)) continue;
    }
    if (app.thumb_cache.count(entry.path)) continue;
    thumb_pool_enqueue(app, entry.path, pending.size);
  }

  // Process the background pre-cache queue (all images in the folder)
  if (app.precache_idx != SIZE_MAX) {
    for (int i = 0; i < AppState::kPrecacheBatchSize; ++i) {
      if (app.precache_idx >= app.precache_paths.size()) {
        app.precache_idx = SIZE_MAX;
        break;
      }
      const std::string& path = app.precache_paths[app.precache_idx++];
      if (app.thumb_cache.find(path) == app.thumb_cache.end()) {
        thumb_pool_enqueue(app, path, 128);
      }
    }
  }

  return !app.thumb_pending_queue.empty() || app.precache_idx != SIZE_MAX;
}

} // namespace eh::file_browser
