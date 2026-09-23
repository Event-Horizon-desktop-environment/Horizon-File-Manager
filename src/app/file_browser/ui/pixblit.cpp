#include "app/file_browser/ui/pixblit.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

#ifdef __SSE2__
#include <emmintrin.h>
#endif
#ifdef __SSSE3__
#include <tmmintrin.h>
#endif

namespace eh::file_browser {

namespace {

// One premultiplied ARGB32 src-over dst pixel, rounded like pixman's 8.8
// blend: out = src + (dst * (255 - srcA) + 0x80) >> 8, per channel,
// saturated to 255 like pixman (packus in the SIMD path below). Saturation
// only triggers for invalid (superluminescent) src pixels; valid
// premultiplied input can never exceed 255, so this is a no-op there.
inline uint32_t over_pixel(uint32_t dst, uint32_t src) {
  const unsigned a = (src >> 24) & 0xffu;
  const unsigned m = 255u - a;
  auto scale = [m](unsigned ch) { return (ch * m + 0x80u) >> 8; };
  auto add_sat = [](unsigned s, unsigned dscaled) {
    unsigned v = s + dscaled;
    return v > 255u ? 255u : v;
  };
  const unsigned sa = (src >> 24) & 0xffu;
  const unsigned da = (dst >> 24) & 0xffu;
  return (add_sat(sa, scale(da)) << 24) |
         (add_sat((src >> 16) & 0xffu, scale((dst >> 16) & 0xffu)) << 16) |
         (add_sat((src >> 8) & 0xffu, scale((dst >> 8) & 0xffu)) << 8) |
         add_sat(src & 0xffu, scale(dst & 0xffu));
}

}  // namespace

void BatchedBlitter::blit_over_row(uint32_t* dst, const uint32_t* src,
                                   int n) const {
#if defined(__SSSE3__)
  const __m128i kZero = _mm_setzero_si128();
  const __m128i kRound = _mm_set1_epi16(0x80);
  const __m128i kFF = _mm_set1_epi8(static_cast<char>(0xff));
  const __m128i kPerm = _mm_setr_epi8(0, 0, 0, 0, 4, 4, 4, 4, 8, 8, 8, 8, 12, 12,
                                      12, 12);
  int i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m128i s =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    // Categorise the whole 16-byte chunk by alpha so tiles of solid icon art
    // and empty label background skip the blend: opaque lanes copy, empty
    // lanes do nothing, only mixed tiles take the real src-over math.
    const __m128i a32 = _mm_srli_epi32(s, 24);       // alpha into byte 0
    const __m128i a = _mm_shuffle_epi8(a32, kPerm);  // alpha in every byte slot
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(a, kZero)) == 0xffff)
      continue;  // fully transparent lane: dst is unchanged
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(a, kFF)) == 0xffff) {
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), s);
      continue;
    }
    const __m128i d =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
    const __m128i m = _mm_sub_epi8(kFF, a);  // 255 - a, already broadcast
    const __m128i slo = _mm_unpacklo_epi8(s, kZero);
    const __m128i shi = _mm_unpackhi_epi8(s, kZero);
    const __m128i dlo = _mm_unpacklo_epi8(d, kZero);
    const __m128i dhi = _mm_unpackhi_epi8(d, kZero);
    const __m128i mlo = _mm_unpacklo_epi8(m, kZero);
    const __m128i mhi = _mm_unpackhi_epi8(m, kZero);
    const __m128i plo = _mm_add_epi16(
        slo, _mm_srli_epi16(
                 _mm_add_epi16(_mm_mullo_epi16(dlo, mlo), kRound), 8));
    const __m128i phi = _mm_add_epi16(
        shi, _mm_srli_epi16(
                 _mm_add_epi16(_mm_mullo_epi16(dhi, mhi), kRound), 8));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i),
                     _mm_packus_epi16(plo, phi));
  }
  for (; i < n; ++i) dst[i] = over_pixel(dst[i], src[i]);
#else
  for (int i = 0; i < n; ++i) dst[i] = over_pixel(dst[i], src[i]);
#endif
}

bool BatchedBlitter::attach(cairo_t* cr, int clip_x, int clip_y, int clip_w,
                            int clip_h) {
  ops_.clear();
  active_ = false;
  dst_ = cairo_get_target(cr);
  if (!dst_ || cairo_surface_status(dst_) != CAIRO_STATUS_SUCCESS) {
    dst_ = nullptr;
    return false;
  }
  cairo_matrix_t m;
  cairo_get_matrix(cr, &m);
  if (m.xx != 1.0 || m.yy != 1.0 || m.xy != 0.0 || m.yx != 0.0 || m.x0 != 0.0 ||
      m.y0 != 0.0)
    return false;
  double ox = 0, oy = 0;
  cairo_surface_get_device_offset(dst_, &ox, &oy);
  if (ox != 0.0 || oy != 0.0) return false;
  double sx = 1, sy = 1;
  cairo_surface_get_device_scale(dst_, &sx, &sy);
  if (sx != 1.0 || sy != 1.0) return false;
  if (cairo_surface_get_type(dst_) != CAIRO_SURFACE_TYPE_IMAGE) return false;
  if (cairo_image_surface_get_format(dst_) != CAIRO_FORMAT_ARGB32) return false;
  data_ = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(dst_));
  if (!data_) return false;
  int stride = cairo_image_surface_get_stride(dst_);
  if (stride <= 0 || (stride & 3) != 0) return false;
  stride_px_ = stride / 4;
  if (clip_w <= 0 || clip_h <= 0) return false;
  clip_x_ = clip_x;
  clip_y_ = clip_y;
  clip_w_ = clip_w;
  clip_h_ = clip_h;
  active_ = true;
  return true;
}

bool BatchedBlitter::add(cairo_surface_t* src, int x, int y) {
  if (!active_ || !src) return false;
  if (cairo_surface_status(src) != CAIRO_STATUS_SUCCESS) return false;
  if (cairo_surface_get_type(src) != CAIRO_SURFACE_TYPE_IMAGE) return false;
  if (cairo_image_surface_get_format(src) != CAIRO_FORMAT_ARGB32) return false;
  double ox = 0, oy = 0;
  cairo_surface_get_device_offset(src, &ox, &oy);
  if (ox != 0.0 || oy != 0.0) return false;
  double sx = 1, sy = 1;
  cairo_surface_get_device_scale(src, &sx, &sy);
  if (sx != 1.0 || sy != 1.0) return false;
  ops_.push_back(Op{src, x, y});
  return true;
}

void BatchedBlitter::flush() {
  if (!active_ || ops_.empty()) return;
  const int clip_x1 = clip_x_ + clip_w_;
  const int clip_y1 = clip_y_ + clip_h_;
  int dirty_x1 = -1, dirty_y1 = -1;  // union of written rects for mark_dirty
  int u_x0 = INT_MAX, u_y0 = INT_MAX;
  for (const Op& op : ops_) {
    const int sw = cairo_image_surface_get_width(op.src);
    const int sh = cairo_image_surface_get_height(op.src);
    if (sw <= 0 || sh <= 0) continue;
    const uint32_t* sp =
        reinterpret_cast<const uint32_t*>(cairo_image_surface_get_data(op.src));
    if (!sp) continue;
    const int ss = cairo_image_surface_get_stride(op.src) / 4;
    int x0 = std::max(op.x, clip_x_);
    int y0 = std::max(op.y, clip_y_);
    int x1 = std::min(op.x + sw, clip_x1);
    int y1 = std::min(op.y + sh, clip_y1);
    if (x1 <= x0 || y1 <= y0) continue;
    for (int yy = y0; yy < y1; ++yy) {
      uint32_t* dp = data_ + yy * stride_px_ + x0;
      const uint32_t* srow = sp + (yy - op.y) * ss + (x0 - op.x);
      blit_over_row(dp, srow, x1 - x0);
    }
    if (x0 < u_x0) u_x0 = x0;
    if (y0 < u_y0) u_y0 = y0;
    if (x1 > dirty_x1) dirty_x1 = x1;
    if (y1 > dirty_y1) dirty_y1 = y1;
  }
  if (u_x0 == INT_MAX) {
    ops_.clear();
    return;
  }
  // Everything outside these op rects was drawn through cairo this frame, so
  // only the union of the raw-blitted rects needs the dirty hint.
  cairo_surface_mark_dirty_rectangle(dst_, u_x0, u_y0, dirty_x1 - u_x0,
                                     dirty_y1 - u_y0);
  ops_.clear();
}

}  // namespace eh::file_browser