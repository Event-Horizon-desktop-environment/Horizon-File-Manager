#pragma once

// hui::layout — measure/allocate layout for immediate-mode UI.
//
// GTK containers measure children (minimum/natural size) then allocate
// rects top-down; paint and input share the allocated rects. This header
// provides the same two-phase model without widgets: build a small node
// tree per surface, measure() it once, place() it into its rect, then
// paint from (and register hits from) node.x/y/w/h. Rebuilt every frame,
// so geometry recalculates itself at any size.
//
// Model:
//   Leaf   — caller-provided natural size (measure your text, set w/h).
//   Column — children stacked vertically, each filled to full width;
//            leftover height goes to flex children proportionally.
//   Row    — children laid horizontally, each filled to full height;
//            leftover width goes to flex children proportionally.
//   Stack  — every child fills the whole rect (overlays, backgrounds).
//
// place() writes x/y/w/h into every node (retained allocations, GTK
// style). Query them for painting and hit registration; never recompute.
//
// Dependency-free; see tests/test_layout.cpp.

#include <vector>

namespace hui {

struct Node {
  enum class Kind { Leaf, Column, Row, Stack };
  Kind kind = Kind::Leaf;
  int w = 0; // Leaf: caller-set natural size. Containers: written by measure().
  int h = 0;
  int gap = 0;  // space between children (Column/Row)
  int flex = 0; // share of leftover space (0 = natural size only)
  std::vector<Node> children;
  int x = 0, y = 0; // written by place()
};

inline int clamp_nonneg(int v) { return v < 0 ? 0 : v; }

// Natural size of the subtree; writes w/h for containers. avail_w bounds
// text-like leaves measured by the caller beforehand (leaves report the
// caller size, clamped to avail_w).
inline void measure(Node& n, int avail_w) {
  if (avail_w < 0) avail_w = 0;
  if (n.kind == Node::Kind::Leaf) {
    if (n.w > avail_w) n.w = avail_w;
    return;
  }
  if (n.kind == Node::Kind::Stack) {
    n.w = 0;
    n.h = 0;
    for (auto& c : n.children) {
      measure(c, avail_w);
      if (c.w > n.w) n.w = c.w;
      if (c.h > n.h) n.h = c.h;
    }
    return;
  }
  if (n.kind == Node::Kind::Column) {
    n.w = 0;
    n.h = 0;
    for (auto& c : n.children) {
      measure(c, avail_w);
      if (c.w > n.w) n.w = c.w;
      n.h += c.h;
    }
    if (!n.children.empty()) n.h += n.gap * (static_cast<int>(n.children.size()) - 1);
    if (n.w > avail_w) n.w = avail_w;
    return;
  }
  // Row.
  n.w = 0;
  n.h = 0;
  for (auto& c : n.children) {
    measure(c, avail_w);
    n.w += c.w;
    if (c.h > n.h) n.h = c.h;
  }
  if (!n.children.empty()) n.w += n.gap * (static_cast<int>(n.children.size()) - 1);
}

// Assign rects top-down; writes x/y/w/h into every node.
inline void place(Node& n, int x, int y, int w, int h) {
  n.x = x;
  n.y = y;
  n.w = w;
  n.h = h;
  if (n.kind == Node::Kind::Leaf || n.children.empty()) return;
  if (n.kind == Node::Kind::Stack) {
    for (auto& c : n.children) place(c, x, y, w, h);
    return;
  }
  if (n.kind == Node::Kind::Column) {
    int fixed = 0;
    int flex_total = 0;
    for (auto& c : n.children) {
      fixed += c.h;
      flex_total += c.flex;
    }
    fixed += n.gap * (static_cast<int>(n.children.size()) - 1);
    int leftover = (flex_total > 0) ? clamp_nonneg(h - fixed) : 0;
    int cy = y;
    for (auto& c : n.children) {
      int ch = c.h + (c.flex > 0 ? leftover * c.flex / flex_total : 0);
      place(c, x, cy, w, ch);
      cy += ch + n.gap;
    }
    return;
  }
  // Row.
  int fixed = 0;
  int flex_total = 0;
  for (auto& c : n.children) {
    fixed += c.w;
    flex_total += c.flex;
  }
  fixed += n.gap * (static_cast<int>(n.children.size()) - 1);
  int leftover = (flex_total > 0) ? clamp_nonneg(w - fixed) : 0;
  int cx = x;
  for (auto& c : n.children) {
    int cw = c.w + (c.flex > 0 ? leftover * c.flex / flex_total : 0);
    place(c, cx, y, cw, h);
    cx += cw + n.gap;
  }
}

} // namespace hui
