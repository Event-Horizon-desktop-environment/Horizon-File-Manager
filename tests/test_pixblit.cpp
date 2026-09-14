// Pixel-correctness test for the batched exact-fit blitter. Renders the same
// deterministic ARGB32 scene twice — once through cairo/pixman (reference)
// and once through eh::file_browser::BatchedBlitter — and requires the raw
// pixel outputs to match within a small rounding tolerance.
//
// Build: meson compile -C build-debug test_pixblit
// Run:   meson test -C build-debug pixblit

#include "app/file_browser/ui/pixblit.hpp"

#include <cairo/cairo.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace fb = eh::file_browser;

static uint32_t* grab(cairo_surface_t* s, int* stride_px) {
  cairo_surface_flush(s);
  *stride_px = cairo_image_surface_get_stride(s) / 4;
  return reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(s));
}

// Deterministic pseudo-random premultiplied ARGB32 (not a real RNG; the point
// is reproducible coverage of many alpha levels and color phases).
static uint32_t mix(uint32_t seed) {
  uint32_t x = seed * 2654435761u + 12345u;
  x ^= x >> 13;
  x *= 0x5bd1e995u;
  x ^= x >> 15;
  const unsigned a = (x >> 16) & 0xffu;   // alpha 0..255
  const unsigned r = (x >> 24) & 0xffu;
  const unsigned g = (x >> 8) & 0xffu;
  const unsigned b = x & 0xffu;
  // premultiply: color channel holds r*a/255
  auto premul = [a](unsigned c) { return static_cast<unsigned>((c * a + 127u) / 255u); };
  return (a << 24) | (premul(r) << 16) | (premul(g) << 8) | premul(b);
}

static int failures = 0;

static void expect_close(uint32_t ref, uint32_t got, int x, int y) {
  const int tol = 2;  // pixman rounding vs ours: at most ±1 per channel
  auto chan = [](uint32_t v, int s) { return static_cast<int>((v >> s) & 0xffu); };
  const int channels[4] = {0, 8, 16, 24};
  for (int s : channels) {
    int d = chan(got, s) - chan(ref, s);
    if (d < -tol || d > tol) {
      std::printf("MISMATCH at (%d,%d): ref=0x%08x got=0x%08x chan_shift=%d diff=%d\n",
                  x, y, ref, got, s, d);
      if (++failures > 40) { std::printf("too many failures, aborting\n"); std::exit(1); }
      return;
    }
  }
}

// Fill a surface with an opaque vertical gradient so dst alpha is exercised.
static void fill_gradient(cairo_surface_t* s) {
  cairo_t* c = cairo_create(s);
  int h = cairo_image_surface_get_height(s);
  cairo_set_source_rgba(c, 0.25, 0.6, 0.85, 1.0);
  cairo_paint(c);
  for (int y = 0; y < h; ++y) {
    double t = static_cast<double>(y) / h;
    cairo_set_source_rgba(c, 0.9 * t, 0.2 + 0.5 * t, 0.4, 1.0);
    cairo_rectangle(c, 0, y, cairo_image_surface_get_width(s), 1);
    cairo_fill(c);
  }
  cairo_destroy(c);
}

int main() {
  constexpr int W = 160, H = 160;
  constexpr int SW = 37, SH = 23;   // odd sizes: unaligned rows/pixels

  // Build a deterministic source with all alpha levels + a few exact cases.
  cairo_surface_t* src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SW, SH);
  int ss_px;
  uint32_t* sp = grab(src, &ss_px);
  for (int y = 0; y < SH; ++y)
    for (int x = 0; x < SW; ++x) sp[y * ss_px + x] = mix(static_cast<uint32_t>(y * SW + x));
  // Content was written straight to the backing buffer: tell cairo it changed
  // so later set_source_surface reads the fresh pixels (image-surface content
  // snapshot, on for source surfaces — skipped only for identity blits).
  cairo_surface_mark_dirty(src);

  auto scene = [&](int dx, int dy, bool batch, int clip_x, int clip_y, int clip_w,
                   int clip_h) {
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    fill_gradient(s);
    if (batch) {
      cairo_t* c = cairo_create(s);
      fb::BatchedBlitter bb;
      if (!bb.attach(c, clip_x, clip_y, clip_w, clip_h)) {
        std::printf("attach failed unexpectedly\n");
        std::exit(1);
      }
      if (!bb.add(src, dx, dy)) {
        std::printf("add failed unexpectedly\n");
        std::exit(1);
      }
      bb.flush();
      cairo_destroy(c);
    } else {
      cairo_t* c = cairo_create(s);
      cairo_rectangle(c, clip_x, clip_y, clip_w, clip_h);
      cairo_clip(c);
      cairo_set_source_surface(c, src, dx, dy);
      cairo_paint(c);
      cairo_destroy(c);
    }
    return s;
  };

  std::printf("pixblit: comparing batched vs cairo...\n");

  // Case 1: fully inside.
  {
    cairo_surface_t* ref = scene(10, 20, false, 0, 0, W, H);
    cairo_surface_t* got = scene(10, 20, true, 0, 0, W, H);
    int rp, gp;
    uint32_t* rd = grab(ref, &rp);
    uint32_t* gd = grab(got, &gp);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) expect_close(rd[y * rp + x], gd[y * gp + x], x, y);
    cairo_surface_destroy(ref);
    cairo_surface_destroy(got);
  }

  // Case 2: partially off-canvas (negative origin) — cairo clips, we clamp.
  {
    cairo_surface_t* ref = scene(-8, H - 21, false, 0, 0, W, H);
    cairo_surface_t* got = scene(-8, H - 21, true, 0, 0, W, H);
    int rp, gp;
    uint32_t* rd = grab(ref, &rp);
    uint32_t* gd = grab(got, &gp);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) expect_close(rd[y * rp + x], gd[y * gp + x], x, y);
    cairo_surface_destroy(ref);
    cairo_surface_destroy(got);
  }

  // Case 3: caller clip narrower than the surface — both must clip identically.
  {
    cairo_surface_t* ref = scene(30, 30, false, 40, 45, 80, 70);
    cairo_surface_t* got = scene(30, 30, true, 40, 45, 80, 70);
    int rp, gp;
    uint32_t* rd = grab(ref, &rp);
    uint32_t* gd = grab(got, &gp);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) expect_close(rd[y * rp + x], gd[y * gp + x], x, y);
    cairo_surface_destroy(ref);
    cairo_surface_destroy(got);
  }

  // Case 4: exact-alpha extremes — fully transparent src must leave dst
  // untouched (within rounding), fully opaque must copy src verbatim.
  {
    cairo_surface_t* ex = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    int exp;
    uint32_t* exd = grab(ex, &exp);
    exd[0 * exp + 0] = 0x00000000u;   // fully transparent
    exd[0 * exp + 1] = 0xFF9A6B3Cu;   // fully opaque
    exd[0 * exp + 2] = 0x80774C3Bu;   // half alpha, premultiplied
    exd[0 * exp + 3] = 0x12A1B2C3u;   // very low alpha
    for (int i = 1; i < 4; ++i)
      for (int j = 0; j < 4; ++j) exd[i * exp + j] = 0xFFFFFFFFu;  // bright row bases
    cairo_surface_mark_dirty(ex);

    cairo_surface_t* ref = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    cairo_surface_t* got = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    fill_gradient(ref);
    fill_gradient(got);
    cairo_t* c = cairo_create(ref);
    cairo_set_source_surface(c, ex, 0, 0);
    cairo_paint(c);
    cairo_destroy(c);
    {
      cairo_t* c2 = cairo_create(got);
      fb::BatchedBlitter bb;
      if (!bb.attach(c2, 0, 0, 4, 4) || !bb.add(ex, 0, 0)) { std::printf("case4 attach/add failed\n"); std::exit(1); }
      bb.flush();
      cairo_destroy(c2);
    }
    int rp2, gp2;
    uint32_t* r2 = grab(ref, &rp2);
    uint32_t* g2 = grab(got, &gp2);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) {
        expect_close(r2[y * rp2 + x], g2[y * gp2 + x], x, y);
        if (x == 1) {  // fully opaque: must be exact
          if (g2[y * gp2 + x] != r2[y * rp2 + x]) {
            std::printf("opaque pixel not exact at (%d,%d): ref=0x%08x got=0x%08x\n",
                        x, y, r2[y * rp2 + x], g2[y * gp2 + x]);
            ++failures;
          }
        }
      }
    cairo_surface_destroy(ex);
    cairo_surface_destroy(ref);
    cairo_surface_destroy(got);
  }

// Case 5: a fully-opaque source must take the copy path (every alpha == 0xff)
  // and produce pixels identical to cairo's src-over of an opaque source.
  {
    cairo_surface_t* op = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 41, 29);
    int op_px;
    uint32_t* opd = grab(op, &op_px);
    for (int y = 0; y < 29; ++y)
      for (int x = 0; x < 41; ++x) {
        unsigned r = static_cast<unsigned>(250 - y * 3), g = static_cast<unsigned>(40 + x * 2), b = static_cast<unsigned>(y * 5);
        opd[y * op_px + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
    cairo_surface_mark_dirty(op);

    auto op_scene = [&](int dx, int dy, bool batch) {
      cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
      fill_gradient(s);
      if (batch) {
        cairo_t* c = cairo_create(s);
        fb::BatchedBlitter bb;
        // tighten the clip so the copy path's x0/y0 clamping is exercised too
        if (!bb.attach(c, 12, 8, W - 24, H - 20) || !bb.add(op, dx, dy)) {
          std::printf("case5 attach/add failed\n"); std::exit(1);
        }
        bb.flush();
        cairo_destroy(c);
      } else {
        cairo_t* c = cairo_create(s);
        cairo_rectangle(c, 12, 8, W - 24, H - 20);
        cairo_clip(c);
        cairo_set_source_surface(c, op, dx, dy);
        cairo_paint(c);
        cairo_destroy(c);
      }
      return s;
    };

    for (int dx : {10, -6}) {
      for (int dy : {25, -3}) {
        cairo_surface_t* ref = op_scene(dx, dy, false);
        cairo_surface_t* got = op_scene(dx, dy, true);
        int rp, gp;
        uint32_t* rd = grab(ref, &rp);
        uint32_t* gd = grab(got, &gp);
        for (int y = 0; y < H; ++y)
          for (int x = 0; x < W; ++x) {
            if (rd[y * rp + x] != gd[y * gp + x]) {
              std::printf("opaque-copy mismatch at (%d,%d): ref=0x%08x got=0x%08x\n",
                          x, y, rd[y * rp + x], gd[y * gp + x]);
              if (++failures > 40) { std::printf("too many failures, aborting\n"); std::exit(1); }
            }
          }
        cairo_surface_destroy(ref);
        cairo_surface_destroy(got);
      }
    }
    cairo_surface_destroy(op);
  }

  std::printf("pixblit: %s\n", failures == 0 ? "OK (all pixels within tolerance)"
                                              : "FAILED");
  cairo_surface_destroy(src);
  return failures == 0 ? 0 : 1;
}