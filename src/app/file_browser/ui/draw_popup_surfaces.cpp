// draw_popup_surfaces.cpp — dedicated layer-shell popups for the context menu
// and the Open With dialog. They used to be painted straight into the main
// window buffer; a layer surface lets us repaint them (hover, scroll, search)
// without repainting the file view, and gives Hyprland a namespace it can
// match with `layerrule = blur, horizon-popup-surface`.

#include "../app.hpp"

#include "app/file_browser/features/popup_position/hyprland_position.hpp"
#include "platform/common/ns/namespaces.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace eh::file_browser {

namespace {

constexpr int kPopupPad = 0;  // layer surface matches the card exactly

struct PopupCtx {
  AppState* app = nullptr;
  OverlayPopupSurface* p = nullptr;
};

void popup_layer_configure(void* data, zwlr_layer_surface_v1* surface,
                           uint32_t serial, uint32_t, uint32_t) {
  auto* ctx = static_cast<PopupCtx*>(data);
  zwlr_layer_surface_v1_ack_configure(surface, serial);
  if (!ctx->p->configured) {
    ctx->p->configured = true;
    ctx->app->pendingRedraw = true;
  }
}

void popup_layer_closed(void* data, zwlr_layer_surface_v1*) {
  auto* ctx = static_cast<PopupCtx*>(data);
  AppState& app = *ctx->app;
  if (ctx->p == &app.contextMenuPopup) {
    app.context_menu_open = false;
    app.context_menu_hover = -1;
    app.context_menu_sub_hover = -1;
  } else if (ctx->p == &app.contextSubmenuPopup) {
    app.context_menu_sub_hover = -1;
  } else if (ctx->p == &app.openWithPopup) {
    app.open_with_open = false;
  }
  app.pendingRedraw = true;
}

constexpr zwlr_layer_surface_v1_listener kLayerListener{
    .configure = popup_layer_configure,
    .closed = popup_layer_closed,
};

void destroy_popup(AppState&, OverlayPopupSurface& p) {
  // Unmap while the layer role is still alive. Attaching a null buffer and
  // committing is what tells the compositor to drop the surface (and with it
  // the blur); destroying the role first leaves the last frame on screen.
  if (p.surface) {
    wl_surface_attach(p.surface, nullptr, 0, 0);
    wl_surface_commit(p.surface);
  }
  p.buf.destroy();
  if (p.blur) {
    ext_background_effect_surface_v1_destroy(p.blur);
    p.blur = nullptr;
  }
  if (p.layer) {
    zwlr_layer_surface_v1_destroy(p.layer);
    p.layer = nullptr;
  }
  if (p.surface) {
    wl_surface_destroy(p.surface);
    p.surface = nullptr;
  }
  delete static_cast<PopupCtx*>(p.ctx);
  p.ctx = nullptr;
  p.configured = false;
  p.req_x = p.req_y = 0;
  p.x = p.y = p.w = p.h = 0;
}

// Layer surfaces are output-anchored, so convert the window-relative popup
// origin into an absolute screen position using Hyprland's IPC.
bool ensure_popup(AppState& app, OverlayPopupSurface& p, int x, int y, int w,
                  int h, int card_w, int card_h) {
  if (!app.wl.compositor() || !app.wl.layer_shell() || !app.surface ||
      !app.shm)
    return false;
  if (w < 1 || h < 1) return false;

  const bool have_surface = p.surface != nullptr;
  const bool geometry_changed =
      have_surface && (p.req_x != x || p.req_y != y || p.w != w || p.h != h);

  // Reuse the existing surface and just update its layer state. Recreating it
  // on every submenu change leaves the previous surface's blur on screen for
  // a frame or two, which made a large submenu's backdrop bleed under the
  // next one.
  if (!have_surface || geometry_changed) {
    WindowRect win{};
    WindowRect mon{};
    const bool have_win = hyprland_window_rect(win);
    if (!have_win) {
      static bool warned = false;
      if (!warned) {
        std::fprintf(stderr,
                     "[horizon-files] warning: could not read window position "
                     "from Hyprland IPC; popups may be misplaced\n");
        warned = true;
      }
      win = WindowRect{0, 0, app.width, app.height};
    }
    if (!hyprland_monitor_for_point(win.x + win.w / 2, win.y + win.h / 2,
                                    mon))
      mon = WindowRect{0, 0, win.w, win.h};

    const int abs_x = win.x + x;
    const int abs_y = win.y + y;
    const int margin_left = abs_x - mon.x;
    const int margin_top = abs_y - mon.y;

    if (!have_surface) {
      wl_surface* surf = wl_compositor_create_surface(app.wl.compositor());
      if (!surf) return false;

      zwlr_layer_surface_v1* layer =
          zwlr_layer_shell_v1_get_layer_surface(
              app.wl.layer_shell(), surf, nullptr,
              ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
              eh::shell::kFileManagerPopupNamespace);
      if (!layer) {
        wl_surface_destroy(surf);
        return false;
      }

      auto* ctx = new PopupCtx{&app, &p};
      zwlr_layer_surface_v1_add_listener(layer, &kLayerListener, ctx);
      zwlr_layer_surface_v1_set_anchor(
          layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
      // -1: ignore bars/reserved areas, so margins are relative to the
      // output origin and arbitrary popup positions are possible.
      zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
      zwlr_layer_surface_v1_set_keyboard_interactivity(
          layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

      p.surface = surf;
      p.layer = layer;
      p.ctx = ctx;
      p.configured = false;

      // Blur region for compositors that honour ext-background-effect-v1 in
      // addition to the layer rule: keep the shadow padding unblurred.
      if (ext_background_effect_manager_v1* mgr =
              app.wl.background_effect_manager()) {
        p.blur =
            ext_background_effect_manager_v1_get_background_effect(mgr, surf);
      }
    }

    zwlr_layer_surface_v1_set_size(p.layer, static_cast<uint32_t>(w),
                                   static_cast<uint32_t>(h));
    zwlr_layer_surface_v1_set_margin(p.layer, margin_top, 0, 0, margin_left);

    p.req_x = x;
    p.req_y = y;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;

    // Only the card itself should receive input; the transparent shadow pad
    // lets clicks through to the main window.
    if (wl_compositor* comp = app.wl.compositor()) {
      wl_region* region = wl_compositor_create_region(comp);
      if (region) {
        wl_region_add(region, kPopupPad, kPopupPad, card_w, card_h);
        wl_surface_set_input_region(p.surface, region);
        wl_region_destroy(region);
      }
    }

    if (p.blur) {
      wl_region* blur_region =
          wl_compositor_create_region(app.wl.compositor());
      if (blur_region) {
        wl_region_add(blur_region, kPopupPad, kPopupPad, card_w, card_h);
        ext_background_effect_surface_v1_set_blur_region(p.blur, blur_region);
        wl_region_destroy(blur_region);
      }
    }

    // An initial buffer-less commit asks the compositor for a configure. When
    // only resizing, the buffer commit that follows applies the new state.
    if (!p.configured) wl_surface_commit(p.surface);
  }
  return true;
}

void commit_popup(AppState& app, OverlayPopupSurface& p, int card_x,
                  int card_y, int card_w, int card_h,
                  const std::function<void(cairo_t*)>& draw) {
  const int pad = kPopupPad;
  const int x = card_x - pad;
  const int y = card_y - pad;
  const int w = card_w + pad * 2;
  const int h = card_h + pad * 2;

  if (!ensure_popup(app, p, x, y, w, h, card_w, card_h))
    return;

  p.card_x = card_x;
  p.card_y = card_y;

  // Draw only after the compositor has configured the layer surface; the
  // configure callback schedules another frame.
  if (!p.configured) return;

  p.buf.ensure(app.shm, eh::shell::kPopupNamespace, w, h);
  cairo_t* cr = p.buf.cairo();
  if (!cr) return;

  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_restore(cr);

  cairo_save(cr);
  cairo_translate(cr, -x, -y);
  draw(cr);
  cairo_restore(cr);

  cairo_surface_flush(p.buf.cairo_surface());

  wl_surface_attach(p.surface, p.buf.wl(), 0, 0);
  wl_surface_damage_buffer(p.surface, 0, 0, w, h);
  p.buf.mark_busy();
  wl_surface_commit(p.surface);
}

}  // namespace

void destroy_context_menu_popup(AppState& app) {
  destroy_popup(app, app.contextMenuPopup);
}

void destroy_context_submenu_popup(AppState& app) {
  destroy_popup(app, app.contextSubmenuPopup);
}

void destroy_open_with_popup(AppState& app) {
  destroy_popup(app, app.openWithPopup);
}

void sync_context_menu_popup(AppState& app) {
  if (!app.context_menu_open) {
    destroy_context_menu_popup(app);
    destroy_context_submenu_popup(app);
    return;
  }
  int x = 0, y = 0, w = 0, h = 0;
  context_menu_popup_bounds(app, x, y, w, h);
  commit_popup(app, app.contextMenuPopup, x, y, w, h,
               [&](cairo_t* cr) { draw_context_menu(app, cr); });
}

void sync_context_submenu_popup(AppState& app) {
  int x = 0, y = 0, w = 0, h = 0;
  context_submenu_popup_bounds(app, x, y, w, h);
  if (!app.context_menu_open || w <= 0 || h <= 0) {
    destroy_context_submenu_popup(app);
    return;
  }
  commit_popup(app, app.contextSubmenuPopup, x, y, w, h,
               [&](cairo_t* cr) { draw_context_submenu(app, cr); });
}

void sync_open_with_popup(AppState& app) {
  if (!app.open_with_open) {
    destroy_open_with_popup(app);
    return;
  }
  int x = 0, y = 0, w = 0, h = 0;
  open_with_popup_rect(app, x, y, w, h);
  commit_popup(app, app.openWithPopup, x, y, w, h,
               [&](cairo_t* cr) { draw_open_with(app, cr); });
}

}  // namespace eh::file_browser
