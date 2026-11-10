// draw_popup_surfaces.cpp — dedicated xdg-popup surfaces for the context menu
// and the Open With dialog. They used to be painted straight into the main
// window buffer; a popup surface lets us repaint them (hover, scroll, search)
// without repainting the file view, and lets Hyprland blur them via
// decoration:blur:popups.

#include "../app.hpp"

#include "platform/common/ns/namespaces.hpp"

#include <algorithm>
#include <functional>

namespace eh::file_browser {

namespace {

constexpr int kPopupPad = 12;  // room for the card drop shadow

// xdg popups deliver configure asynchronously; this links the protocol
// callbacks back to the owning AppState and popup slot.
struct PopupCtx {
  AppState* app = nullptr;
  OverlayPopupSurface* p = nullptr;
};

void popup_xdg_surface_configure(void* data, xdg_surface* surface,
                                 uint32_t serial) {
  auto* ctx = static_cast<PopupCtx*>(data);
  xdg_surface_ack_configure(surface, serial);
  ctx->app->pendingRedraw = true;
}

void popup_configure(void* data, xdg_popup*, int32_t x, int32_t y, int32_t,
                     int32_t) {
  auto* ctx = static_cast<PopupCtx*>(data);
  OverlayPopupSurface& p = *ctx->p;
  if (x != p.x || y != p.y) {
    p.x = x;
    p.y = y;
  }
  if (!p.configured) {
    p.configured = true;
    ctx->app->pendingRedraw = true;
  }
}

void popup_done(void* data, xdg_popup*) {
  auto* ctx = static_cast<PopupCtx*>(data);
  AppState& app = *ctx->app;
  if (ctx->p == &app.contextMenuPopup) {
    app.context_menu_open = false;
    app.context_menu_hover = -1;
    app.context_menu_sub_hover = -1;
  } else if (ctx->p == &app.openWithPopup) {
    app.open_with_open = false;
  }
  app.pendingRedraw = true;
}

constexpr xdg_surface_listener kPopupXdgSurfaceListener{
    .configure = popup_xdg_surface_configure,
};

constexpr xdg_popup_listener kPopupListener{
    .configure = popup_configure,
    .popup_done = popup_done,
    .repositioned = [](void*, xdg_popup*, uint32_t) {},
};

void destroy_popup(AppState&, OverlayPopupSurface& p) {
  p.buf.destroy();
  if (p.xdgPopup) {
    xdg_popup_destroy(p.xdgPopup);
    p.xdgPopup = nullptr;
  }
  if (p.xdgSurface) {
    xdg_surface_destroy(p.xdgSurface);
    p.xdgSurface = nullptr;
  }
  if (p.blur) {
    ext_background_effect_surface_v1_destroy(p.blur);
    p.blur = nullptr;
  }
  if (p.surface) {
    wl_surface_attach(p.surface, nullptr, 0, 0);
    wl_surface_commit(p.surface);
    wl_surface_destroy(p.surface);
    p.surface = nullptr;
  }
  delete static_cast<PopupCtx*>(p.ctx);
  p.ctx = nullptr;
  p.configured = false;
  p.req_x = p.req_y = 0;
  p.x = p.y = p.w = p.h = 0;
}

// Surface geometry is fixed for the lifetime of one popup position: moving
// or resizing recreates the popup, which is cheap and avoids stale buffer
// coordinates.
bool ensure_popup(AppState& app, OverlayPopupSurface& p, int x, int y, int w,
                  int h, int card_w, int card_h) {
  if (!app.wl.compositor() || !app.wl.xdg_base() || !app.xdgSurface ||
      !app.surface || !app.shm)
    return false;
  if (w < 1 || h < 1) return false;

  if (p.surface && (p.req_x != x || p.req_y != y || p.w != w || p.h != h))
    destroy_popup(app, p);

  if (!p.surface) {
    wl_surface* surf = wl_compositor_create_surface(app.wl.compositor());
    if (!surf) return false;

    xdg_surface* xdg =
        xdg_wm_base_get_xdg_surface(app.wl.xdg_base(), surf);
    if (!xdg) {
      wl_surface_destroy(surf);
      return false;
    }

    auto* ctx = new PopupCtx{&app, &p};
    xdg_surface_add_listener(xdg, &kPopupXdgSurfaceListener, ctx);

    xdg_positioner* pos = xdg_wm_base_create_positioner(app.wl.xdg_base());
    if (!pos) {
      delete ctx;
      xdg_surface_destroy(xdg);
      wl_surface_destroy(surf);
      return false;
    }
    // Anchor the popup's top-left corner exactly at (x, y) in the main
    // window's coordinate space. No constraint adjustment: the card is
    // already clamped to the window, and exact placement keeps our input
    // translation and drawing coordinates identical.
    xdg_positioner_set_size(pos, w, h);
    xdg_positioner_set_anchor_rect(pos, 0, 0, 1, 1);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(pos, x, y);
    xdg_positioner_set_constraint_adjustment(pos, 0);

    xdg_popup* popup =
        xdg_surface_get_popup(xdg, app.xdgSurface, pos);
    xdg_positioner_destroy(pos);
    if (!popup) {
      delete ctx;
      xdg_surface_destroy(xdg);
      wl_surface_destroy(surf);
      return false;
    }
    xdg_popup_add_listener(popup, &kPopupListener, ctx);

    xdg_surface_set_window_geometry(xdg, 0, 0, w, h);

    p.surface = surf;
    p.xdgSurface = xdg;
    p.xdgPopup = popup;
    p.ctx = ctx;
    p.req_x = x;
    p.req_y = y;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.configured = false;

    // Only the card itself should receive input; the transparent shadow pad
    // lets clicks through to the main window.
    if (wl_compositor* comp = app.wl.compositor()) {
      wl_region* region = wl_compositor_create_region(comp);
      if (region) {
        wl_region_add(region, kPopupPad, kPopupPad, card_w, card_h);
        wl_surface_set_input_region(surf, region);
        wl_region_destroy(region);
      }
    }

    // Ask the compositor to blur whatever is behind the popup card. Hyprland
    // supports this on xdg popups, and the region keeps the blur off the
    // transparent shadow padding.
    if (ext_background_effect_manager_v1* mgr =
            app.wl.background_effect_manager()) {
      p.blur = ext_background_effect_manager_v1_get_background_effect(mgr, surf);
      if (p.blur) {
        wl_region* blur_region =
            wl_compositor_create_region(app.wl.compositor());
        if (blur_region) {
          wl_region_add(blur_region, kPopupPad, kPopupPad, card_w, card_h);
          ext_background_effect_surface_v1_set_blur_region(p.blur, blur_region);
          wl_region_destroy(blur_region);
        }
      }
    }

    // Initial commit with no buffer: asks the compositor for a configure.
    wl_surface_commit(p.surface);
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

  // Draw only after the compositor has configured the popup; the configure
  // callback schedules another frame.
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

void destroy_open_with_popup(AppState& app) {
  destroy_popup(app, app.openWithPopup);
}

void sync_context_menu_popup(AppState& app) {
  if (!app.context_menu_open) {
    destroy_context_menu_popup(app);
    return;
  }
  int x = 0, y = 0, w = 0, h = 0;
  context_menu_popup_bounds(app, x, y, w, h);
  commit_popup(app, app.contextMenuPopup, x, y, w, h,
               [&](cairo_t* cr) { draw_context_menu(app, cr); });
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
