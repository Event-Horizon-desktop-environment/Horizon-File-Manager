#pragma once

#include "../app.hpp"

namespace eh::file_browser {

// ── Input module (input/*.cpp) ───────────────────────────────────
// Shared input-module surface. The event entry points (handle_click,
// handle_key, handle_scroll, handle_pointer_move, handle_pointer_release,
// properties_hit_test, settings_hit_test) are declared in app.hpp and
// implemented in frequency/called per-handler TUs:
//
//   click.cpp           handle_click dispatcher, properties_hit_test,
//                       settings_hit_test
//   click_dialogs.cpp   dialog-region click handlers (drop chooser through
//                       terminal chooser)
//   click_chrome.cpp    modal_blocks_content + chrome-region click handlers
//                       (scrollbar through column header, columns/sort menus)
//   click_views.cpp     pane/content + right-click-region handlers
//   keyboard.cpp        handle_key dispatcher + grouped-header row helpers
//                       (key_* handlers live in key_dialog/key_edit/
//                       key_shortcuts/key_nav; the dispatcher keeps the
//                       prologue, split-focus block, and the shared
//                       show_hidden_passthrough computation)
//   pointer.cpp  handle_pointer_move
//   scroll.cpp   handle_scroll
//   release.cpp  handle_pointer_release
//   events.cpp   apply_scrollbar_drag (shared by click + pointer)

// Constants matching draw.cpp for filter dropdown layout (used by both the
// click and pointer handlers so the two can never disagree about the dropdown
// geometry).
static constexpr int kFilterHdrH = 28;
static constexpr int kFilterItemH = 24;
static constexpr int kFilterPD = 6;
static constexpr int kFilterSep = 4;

// Main-view scrollbar: maps pointer Y to scroll position (thumb grab /
// tap-to-jump). Called by handle_click (initial grab/tap) and
// handle_pointer_move (thumb drag); the grab offset captured at grab time is
// applied here. Defined in events.cpp.
void apply_scrollbar_drag(AppState& app, int y);

// ── Click region handlers (click_dialogs.cpp) ────────────────────
// Each returns true exactly where handle_click used to `return;`, i.e. the
// click was consumed. handle_click dispatches to them in this order.
bool click_drop_chooser(AppState& app, int x, int y, int button);
bool click_context_menu(AppState& app, int x, int y, int button);
bool click_split_pane(AppState& app, int x, int y, int button);
bool click_scrollbar(AppState& app, int x, int y, int button);
bool click_path_edit_cancel(AppState& app, int x, int y, int button);
bool click_status_zoom(AppState& app, int x, int y, int button);
bool click_picker_bar(AppState& app, int x, int y, int button);
bool click_info_tab(AppState& app, int x, int y, int button);
bool click_sidebar_drag(AppState& app, int x, int y, int button);
bool click_properties(AppState& app, int x, int y, int button);
bool click_conflict(AppState& app, int x, int y, int button);
bool click_confirm(AppState& app, int x, int y, int button);
bool click_password(AppState& app, int x, int y, int button);
bool click_compress(AppState& app, int x, int y, int button);
bool click_select_pattern(AppState& app, int x, int y, int button);
bool click_create(AppState& app, int x, int y, int button);
bool click_rename_ui(AppState& app, int x, int y, int button);
bool click_batch_rename(AppState& app, int x, int y, int button);
bool click_open_with(AppState& app, int x, int y, int button);
bool click_term_chooser(AppState& app, int x, int y, int button);

// ── Chrome-region click handlers (click_chrome.cpp) ──────────────
bool click_columns_menu(AppState& app, int x, int y, int button);
bool click_sort_menu(AppState& app, int x, int y, int button);
bool click_filter_dropdown(AppState& app, int x, int y, int button);
bool click_top_bar(AppState& app, int x, int y, int button);
bool click_tab_bar(AppState& app, int x, int y, int button);
bool click_ops_cancel(AppState& app, int x, int y, int button);
bool click_flap_swallow(AppState& app, int x, int y, int button);
bool click_column_header(AppState& app, int x, int y, int button);

// ── Pane/content + right-click-region handlers (click_views.cpp) ─
bool click_sidebar_hit(AppState& app, int x, int y, int button);
bool click_content_hit(AppState& app, int x, int y, int button,
                       uint64_t now_ns);
bool click_rpath_edit(AppState& app, int x, int y, int button);
bool click_rtab_bar(AppState& app, int x, int y, int button);
bool click_rcomputer(AppState& app, int x, int y, int button);
bool click_rctx_close(AppState& app, int x, int y, int button);
bool click_rsidebar(AppState& app, int x, int y, int button);
bool click_rcontent(AppState& app, int x, int y, int button);

// ── Key-region handlers ──────────────────────────────────────────
// handle_key dispatches to these in flow order; each returns true exactly
// where handle_key used to `return true`, i.e. the key was consumed.
bool key_properties(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                    const char* utf8, int utf8_len);
bool key_cancel_op(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                   const char* utf8, int utf8_len);
bool key_confirm(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                 const char* utf8, int utf8_len);
bool key_settings(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                  const char* utf8, int utf8_len);
bool key_select_pattern(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                        const char* utf8, int utf8_len);
bool key_compress(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                  const char* utf8, int utf8_len);
bool key_create(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                const char* utf8, int utf8_len);
bool key_password(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                  const char* utf8, int utf8_len);
bool key_rename_ui(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                   const char* utf8, int utf8_len);
bool key_batch_rename(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                      const char* utf8, int utf8_len);
bool key_term_chooser(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                      const char* utf8, int utf8_len);
bool key_search(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                const char* utf8, int utf8_len, bool show_hidden_passthrough);
bool key_path_edit(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                   const char* utf8, int utf8_len, bool show_hidden_passthrough);
bool key_global_shortcuts(AppState& app, uint32_t sym, bool ctrl, bool shift,
                          bool alt, const char* utf8, int utf8_len);
bool key_navigate(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                  const char* utf8, int utf8_len);
bool key_text_fallback(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                       const char* utf8, int utf8_len);
bool key_type_to_find(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                      const char* utf8, int utf8_len);
bool key_space_dismiss(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                       const char* utf8, int utf8_len);
bool key_drop_dismiss(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                      const char* utf8, int utf8_len);
bool key_escape_clear(AppState& app, uint32_t sym, bool ctrl, bool shift, bool alt,
                      const char* utf8, int utf8_len);

} // namespace eh::file_browser