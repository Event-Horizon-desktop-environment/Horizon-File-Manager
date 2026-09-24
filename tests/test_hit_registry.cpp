// test_hit_registry.cpp — unit tests for ui/hit_registry.hpp.
// Pure logic, no Wayland/cairo needed.

#include "ui/hit.hpp"

#include <cassert>
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

int main() {
  // Empty registry misses.
  {
    hui::HitRegistry r;
    CHECK(r.query(0, 0) == hui::Hit::kNone);
    CHECK(r.size() == 0);
  }

  // Basic hit / edge exclusivity (right/bottom edges are exclusive).
  {
    hui::HitRegistry r;
    r.add(hui::Hit::tab(2), 100, 56, 120, 44);
    CHECK(r.query(100, 56) == hui::Hit::tab(2));
    CHECK(r.query(219, 99) == hui::Hit::tab(2));
    CHECK(r.query(220, 56) == hui::Hit::kNone);
    CHECK(r.query(100, 100) == hui::Hit::kNone);
    CHECK(r.query(99, 99) == hui::Hit::kNone);
  }

  // Topmost (last registered) wins on overlap — paint-on-top semantics.
  {
    hui::HitRegistry r;
    r.add(hui::Hit::sidebar_row(0), 0, 56, 200, 43);   // row underneath
    r.add(hui::Hit::tab(0), 0, 56, 300, 44);           // painted later, on top
    CHECK(r.query(50, 70) == hui::Hit::tab(0));
    // Outside the tab but inside the row: row still hittable.
    r.clear();
    r.add(hui::Hit::sidebar_row(0), 0, 100, 200, 43);
    r.add(hui::Hit::tab(0), 0, 56, 300, 44);
    CHECK(r.query(50, 120) == hui::Hit::sidebar_row(0));
  }

  // Group matching via mask, index extraction.
  {
    hui::HitRegistry r;
    r.add(hui::Hit::tab_close(5), 10, 10, 20, 20);
    uint32_t hid = r.query(15, 15);
    CHECK((hid & hui::Hit::kGroupMask) == hui::Hit::kTabClose);
    CHECK((hid & hui::Hit::kIndexMask) == 5);
    r.clear();
    r.add(hui::Hit::sidebar_row(9), 0, 0, 100, 43);
    hid = r.query(4, 4);
    CHECK((hid & hui::Hit::kGroupMask) == hui::Hit::kSidebarRow);
    CHECK((hid & hui::Hit::kIndexMask) == 9);
  }

  // Degenerate rects are never registered.
  {
    hui::HitRegistry r;
    r.add(hui::Hit::tab(0), 10, 10, 0, 44);
    r.add(hui::Hit::tab(1), 10, 10, 44, 0);
    r.add(hui::Hit::tab(2), 10, 10, -5, 44);
    r.add(hui::Hit::kNone, 10, 10, 44, 44);
    CHECK(r.size() == 0);
    CHECK(r.query(10, 10) == hui::Hit::kNone);
  }

  // Resize scenario: re-registering after clear reflects new geometry.
  // (The regression that swallowed "My Computer": stale band math.)
  {
    hui::HitRegistry r;
    r.add(hui::Hit::sidebar_row(0), 0, 56, 200, 43); // narrow window
    CHECK(r.query(150, 70) == hui::Hit::sidebar_row(0));
    r.clear(); // next frame at a new size
    r.add(hui::Hit::sidebar_row(0), 0, 56, 320, 43); // wider window
    CHECK(r.query(300, 70) == hui::Hit::sidebar_row(0));
    CHECK(r.query(150, 70) == hui::Hit::sidebar_row(0));
  }

  // find() returns the region for cursor math, nullptr on miss.
  {
    HitRegistry r;
    r.add(Hit::dialog(Hit::kDlgCreate, Hit::kCreateInput), 20, 50, 300, 34);
    const HitRegion* f = r.find(30, 60);
    CHECK(f != nullptr);
    CHECK(f->x == 20 && f->w == 300);
    CHECK(r.find(0, 0) == nullptr);
    const HitRegion* g = r.find_id(Hit::dialog(Hit::kDlgCreate, Hit::kCreateInput));
    CHECK(g != nullptr && g->y == 50 && g->h == 34);
    CHECK(r.find_id(Hit::dialog(Hit::kDlgCreate, Hit::kCreateOk)) == nullptr);
  }

  // Top-bar pane ids stay distinct from chrome groups.
  {
    uint32_t a = hui::Hit::topbar(0, hui::Hit::kTopGear);
    uint32_t b = hui::Hit::topbar(1, hui::Hit::kTopGear);
    CHECK(a != b);
    CHECK((a & hui::Hit::kGroupMask) == hui::Hit::kTopBar);
    CHECK((b & hui::Hit::kGroupMask) == hui::Hit::kTopBar);
  }

  // Menu ids round-trip through menu_id()/menu_row(). The group base
  // occupies the high bits, so a raw (hid >> 8) & 0xFF shift does NOT
  // yield the menu number — this regression test pins the decoders.
  {
    const int menus[] = {hui::Hit::kMenuCtx,     hui::Hit::kMenuCtxSub, hui::Hit::kMenuSort,
                         hui::Hit::kMenuColumns, hui::Hit::kMenuFilter, hui::Hit::kMenuDrop};
    for (int m : menus) {
      for (int row : {0, 1, 200, 255, 300, 1023}) {
        uint32_t hid = hui::Hit::menu(m, row);
        CHECK((hid & hui::Hit::kGroupMask) == hui::Hit::kMenu);
        CHECK(hui::Hit::menu_id(hid) == m);
        CHECK(hui::Hit::menu_row(hid) == row);
      }
    }
    // Registry end-to-end: overlapping rows resolve topmost-first.
    HitRegistry r;
    r.add(hui::Hit::menu(hui::Hit::kMenuCtx, 3), 100, 100, 200, 34);
    CHECK(hui::Hit::menu_id(r.query(150, 110)) == hui::Hit::kMenuCtx);
    CHECK(hui::Hit::menu_row(r.query(150, 110)) == 3);
    CHECK(r.query(0, 0) == 0);
  }

  // View rows exceed 11 bits in large folders: index 2048+ must not alias
  // the expander group or neighboring groups.
  {
    uint32_t vr = hui::Hit::view_row(5000);
    uint32_t va = hui::Hit::view_arrow(5000);
    CHECK((vr & hui::Hit::kGroupMask) == hui::Hit::kViewRow);
    CHECK((va & hui::Hit::kGroupMask) == hui::Hit::kViewArrow);
    CHECK((vr & hui::Hit::kIndexMask) == 5000);
    CHECK((va & hui::Hit::kIndexMask) == 5000);
    CHECK(vr != hui::Hit::view_arrow(0));
    CHECK((vr & hui::Hit::kGroupMask) != hui::Hit::kMenu);
    CHECK((vr & hui::Hit::kGroupMask) != hui::Hit::kDialog);
    CHECK((vr & hui::Hit::kGroupMask) != hui::Hit::kTopBar);
  }

  // Dialog controls exceed 8 bits in long app lists: control 256+ must
  // stay in its dialog and decode exactly.
  {
    uint32_t hid = hui::Hit::dialog(hui::Hit::kDlgOpenWith, hui::Hit::kOpenRowBase + 300);
    CHECK((hid & hui::Hit::kGroupMask) == hui::Hit::kDialog);
    CHECK((hid & 0xFFFFC00u) == hui::Hit::dialog(hui::Hit::kDlgOpenWith, 0));
    CHECK(hui::Hit::dialog_ctrl(hid) == hui::Hit::kOpenRowBase + 300);
    CHECK(hid != hui::Hit::dialog(hui::Hit::kDlgBatch, 0));
  }

  if (failures == 0) std::printf("hit_registry: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
