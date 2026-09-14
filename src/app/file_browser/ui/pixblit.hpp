#pragma once

#include <cairo/cairo.h>

#include <cstdint>
#include <vector>

namespace eh::file_browser {

// Accumulates exact-fit ARGB32 blits and composites them in a single pass
// over the destination surface's raw pixels. Skips cairo/pixman per-op
// overhead (measured ~7 µs per small blit at 4K grid density) by running one
// region/lookup-free tight loop for the whole batch.
//
// SAFETY: attach() only activates when the destination is a
// CAIRO_FORMAT_ARGB32 image surface with an identity cairo transform and no
// device offset/scale, so the raw-data mapping matches exactly what cairo
// would draw. Each add() verifies the source; anything the fast path cannot
// represent returns false and the caller MUST fall back to a plain cairo
// paint for that op.
class BatchedBlitter {
public:
  BatchedBlitter() = default;
  BatchedBlitter(const BatchedBlitter&) = delete;
  BatchedBlitter& operator=(const BatchedBlitter&) = delete;

  // Attempt to attach to cr. clip{x,y,w,h} is the cairo clip rectangle the
  // caller is drawing into (cr user-space == surface plane); this class
  // honours it the same way the clip would.
  bool attach(cairo_t* cr, int clip_x, int clip_y, int clip_w, int clip_h);
  bool active() const { return active_; }

  // Queue an exact-fit blit of src at (x,y). Returns true when queued; false
  // means the caller must draw this op through cairo instead.
  bool add(cairo_surface_t* src, int x, int y);

  // Composite all queued ops into the destination and clear the queue. Must
  // be called before further cairo drawing on cr.
  void flush();

private:
  static_assert(sizeof(uint32_t) == 4, "ARGB32 needs 32-bit words");

  void blit_over_row(uint32_t* dst, const uint32_t* src, int n) const;

  cairo_surface_t* dst_ = nullptr;
  uint32_t* data_ = nullptr;
  int stride_px_ = 0;          // destination rows in 32-bit words
  int clip_x_ = 0, clip_y_ = 0, clip_w_ = 0, clip_h_ = 0;
  bool active_ = false;

  struct Op {
    cairo_surface_t* src;
    int x, y;
  };
  std::vector<Op> ops_;
};

}  // namespace eh::file_browser