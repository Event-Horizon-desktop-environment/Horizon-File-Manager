# Hit testing: never fix hit positions again

## The rule

**Input code may never contain geometry. Paint, hits and input share one
layout.**

`src/ui/` is the new UI system (framework, not a rewrite: adopted surface
by surface, see below):

- `ui/layout.hpp` — measure/allocate nodes (Column/Row/Stack/Leaf).
  Build a small tree per surface, `measure()` once, `place()` into its
  rect. Paint from node rects; register hits from node rects.
- `ui/hit.hpp` — retained hit registry (moved here from
  `app/file_browser/ui/`). Cleared every paint; draw sites register the
  exact rects they paint; handlers `query()`/`find()`.
- `ui/design.hpp` — visual atoms (unchanged language).
- `ui/actions.hpp` — named action registry (next: menus/shortcuts render
  from it instead of triplicating predicates).

Every clickable thing is registered at its draw site from the same
locals (or layout nodes) used for painting. Resize, zoom, scroll, fold,
split — the next paint recalculates everything. There is no parallel
geometry left to drift.

## Why this is the fix (and what KDE/GNOME do)

- **Nautilus (GTK)** delivers pointer events with `gtk_widget_pick()`,
  which walks the widget tree testing each widget's *allocation* — the
  rect assigned once per layout pass in `size_allocate()`. Paint
  (snapshot) and events read the same rects.
- **Dolphin (Qt)** does the same through item geometries and `childAt()`.
- Our registry emulates exactly that for immediate-mode cairo: paint
  writes allocations, input picks through them (last-registered =
  topmost wins, mirroring paint order).

The bug class this kills: draw code and input code each re-deriving the
same rect (sidebar rows vs `hit_test_sidebar`, tab band vs tab rects).
The "My Computer" row sat at y 56–99 inside the tab band y 56–100 while
the tab handler swallowed the whole band — invisible until clicked.

## Using it

```cpp
// At the draw site (same locals you paint with):
app.hit_main.add(Hit::sidebar_row(idx), 0, y, sidebar_w, item_h);

// In the input handler:
uint32_t hid = app.hit_main.query(x, y);
if ((hid & Hit::kGroupMask) != Hit::kSidebarRow) return -1;
int idx = static_cast<int>(hid & Hit::kIndexMask);
```

IDs live in `Hit::` (`hit_registry.hpp`): groups `kTab`, `kTabClose`,
`kSidebarRow`, `kSidebarFavAdd`, `kTopBar` (+ pane bit + control
number for split-view bars). Add new controls at the end of the
relevant range; never reuse a value.

## Verifying

- `meson test -C build-debug hit_registry` — registry semantics
  (z-order, edges, degenerate rects, resize re-registration).
- `EH_HIT_DEBUG=1 horizon-files` — magenta outlines over every live
  region. Resize the window: outlines must track what you see.

## Migrated so far

- Tab bar body + close zones (left/middle/right click) — band math deleted.
- Sidebar rows (click, hover, drag-drop targeting) — 60-line parallel
  layout deleted. Bonus fix: the "Add to Favorites" row no longer
  mis-hits the first drive (it was invisible to the old math).
- Create dialog (first `src/ui` proof): layout nodes place every rect,
  paint/hits/click/hover all derive from them — the "must match
  draw_create_dialog" comment block is gone.
- Rename, confirm, password dialogs: same recipe (layout nodes for
  inputs/buttons, registry click/hover; symmetric button margins).
- Compress dialog: full column layout incl. chip rows; click/hover decode
  chip indices from IDs (unavailable formats still swallow).
- Terminal chooser: rows registered per visible index; click/hover decode.
- Open-With list: rows registered; buttons already read stored rects.
- Batch rename: tabs/fields/add/dropdown/buttons registered at draw
  sites; click/hover decode (dropdown-outside still closes, then the
  click keeps processing).
- Conflict: checkbox/buttons registered; click reads the registry
  (also fixes it using event coords instead of stale pointer state).
- Settings window: own registry (`hit_settings`, separate coordinate
  space); all 20+ controls registered at draw sites; the 250-line
  parallel-geometry `settings_hit_test` is now a query-to-code map
  (return contract unchanged, zoom-edit side effect moved to the click
  handler where it belongs).
- Top bar: nav arrows, fold toggle, gear and path field/text registered
  per pane (fixes split-view hover acting on the wrong pane); click and
  hover decode IDs. Path-edit cursor math reads the text rect (also
  fixing clicks mapping against a different width than paint scrolls by).
- Views: list/grid/tree/compact rows + tree expanders + computer cards
  registered before visibility culling (scroll-reuse frames stay
  complete); all five hit tests are queries. Bonus fixes: grouped-grid
  clicks (legacy ignored group bands), computer-view split picking
  (legacy always used main-pane geometry), fav-drag slots (72px off).
- Menus: context/submenu, sort (+card), columns, filter (headers/items),
  drop chooser — all registered; handlers decode (filter glob mapping
  uses counts only, no pixels).
- Column headers: segments + dividers registered; click decodes.
- Fav-drag insertion: computed from painted row tops.
- Still on legacy math (same recipe applies): top-bar controls
  (IDs reserved: `Hit::kTop*`), fav-section drop range, view rows,
  computer grid, remaining dialogs, menus. Migrate by building layout
  nodes, registering at the draw site, switching the handler to
  `query()`, deleting the old math.
