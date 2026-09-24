// test_layout.cpp — unit tests for ui/layout.hpp (measure/allocate).

#include "ui/layout.hpp"

#include <cstdio>

using namespace hui;

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                          \
  } while (0)

static Node leaf(int w, int h, int flex = 0) {
  Node n;
  n.kind = Node::Kind::Leaf;
  n.w = w;
  n.h = h;
  n.flex = flex;
  return n;
}

int main() {
  // Leaf reports caller size, clamped to availability.
  {
    Node n = leaf(300, 34);
    measure(n, 200);
    CHECK(n.w == 200 && n.h == 34);
  }

  // Column stacks full-width children with gaps; flex absorbs leftover.
  {
    Node col;
    col.kind = Node::Kind::Column;
    col.gap = 6;
    col.children.push_back(leaf(300, 30));
    col.children.push_back(leaf(300, 34));
    Node spacer = leaf(0, 0);
    spacer.flex = 1;
    col.children.push_back(std::move(spacer));
    col.children.push_back(leaf(90, 32));
    measure(col, 300);
    CHECK(col.w == 300);
    CHECK(col.h == 30 + 6 + 34 + 6 + 0 + 6 + 32);
    place(col, 20, 14, 300, 128);
    CHECK(col.children[0].x == 20 && col.children[0].y == 14);
    CHECK(col.children[0].w == 300 && col.children[0].h == 30);
    CHECK(col.children[1].y == 14 + 30 + 6);
    // Fixed total = 30+34+0+32 + 3*6 = 114; leftover 14 -> spacer.
    CHECK(col.children[2].h == 14);
    CHECK(col.children[3].y + col.children[3].h == 14 + 128);
    CHECK(col.children[3].w == 300);
  }

  // Row: leftover width goes to flex children; heights fill.
  {
    Node row;
    row.kind = Node::Kind::Row;
    Node spacer = leaf(0, 0);
    spacer.flex = 1;
    row.children.push_back(std::move(spacer));
    row.children.push_back(leaf(90, 32));
    row.children.push_back(leaf(90, 32));
    measure(row, 300);
    CHECK(row.h == 32);
    place(row, 0, 110, 300, 32);
    // Fixed = 180; leftover 120 -> spacer.
    CHECK(row.children[0].w == 120 && row.children[0].x == 0);
    CHECK(row.children[1].x == 120 && row.children[1].w == 90);
    CHECK(row.children[2].x == 210 && row.children[2].w == 90);
    CHECK(row.children[1].h == 32);
  }

  // Row gap support.
  {
    Node row;
    row.kind = Node::Kind::Row;
    row.gap = 20;
    Node spacer = leaf(0, 0);
    spacer.flex = 1;
    row.children.push_back(std::move(spacer));
    row.children.push_back(leaf(90, 32));
    row.children.push_back(leaf(90, 32));
    place(row, 0, 0, 300, 32);
    // Fixed = 180 + 2*20 = 220; leftover 80 -> spacer.
    CHECK(row.children[0].w == 80);
    CHECK(row.children[1].x == 80 + 20);
    CHECK(row.children[2].x == 80 + 20 + 90 + 20);
  }

  // Stack fills every child.
  {
    Node st;
    st.kind = Node::Kind::Stack;
    st.children.push_back(leaf(10, 10));
    st.children.push_back(leaf(50, 60));
    measure(st, 500);
    CHECK(st.w == 50 && st.h == 60);
    place(st, 5, 5, 100, 100);
    CHECK(st.children[0].x == 5 && st.children[0].w == 100);
    CHECK(st.children[1].y == 5 && st.children[1].h == 100);
  }

  // No flex: overflow keeps natural sizes (caller scrolls).
  {
    Node col;
    col.kind = Node::Kind::Column;
    col.children.push_back(leaf(100, 200));
    col.children.push_back(leaf(100, 200));
    measure(col, 500);
    place(col, 0, 0, 500, 100);
    CHECK(col.children[0].h == 200 && col.children[1].h == 200);
  }

  if (failures == 0) std::printf("layout: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
