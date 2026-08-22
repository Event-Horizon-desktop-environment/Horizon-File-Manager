#pragma once

#include "app/file_browser/app_types.hpp"

#include <string>
#include <unordered_set>

#include <cairo/cairo.h>

namespace eh::file_browser {

/// Main draw function — paints the entire file browser UI (standalone, uses own buffers + surface).
void draw(AppState& app);

/// Paint the file browser UI into an existing cairo context (embedded mode, no Wayland surface required).
void paint(AppState& app, cairo_t* cr);

/// Schedule the next frame if animations or thumbs are pending.
void schedule_frame(AppState& app);

/// Handle a pointer button press.
void handle_click(AppState& app, int x, int y, int button);

/// Handle a pointer button release.
void handle_pointer_release(AppState& app, int x, int y, int button);

/// Handle pointer motion.
void handle_pointer_move(AppState& app, int x, int y);

/// Handle scroll (positive dy = scroll down).
void handle_scroll(AppState& app, int x, int y, double dx, double dy);

/// Handle a key press/release.
/// Returns true if the key was consumed.
bool handle_key(AppState& app, uint32_t keycode, uint32_t state,
                xkb_keysym_t sym, const char* utf8, int utf8_len);

/// Navigate to a directory. Updates history, reloads listing.
void navigate_to(AppState& app, const std::string& path);

/// Navigate to the parent directory.
void navigate_up(AppState& app);

/// Go back in history.
void navigate_back(AppState& app);

/// Go forward in history.
void navigate_forward(AppState& app);

/// Reload the current directory listing.
void reload_dir(AppState& app);
void join_scan(AppState& app);
void apply_scan_result(AppState& app, bool is_progress = false);
std::string group_label_for(const AppState& app, const FileEntry& e);
std::string format_mode(uint32_t mode);

// Names listed in a directory's `.hidden` file (freedesktop convention).
std::unordered_set<std::string> read_hidden_file(const std::string& dir);

// Thread-safe filter-dropdown predicate for the search worker (type/size/date
// indices; 0 = any). Implemented in nav.cpp; runs on the worker thread.
bool search_predicate_passes(int type_idx, int size_idx, int date_idx,
                             const std::string& path, const std::string& name,
                             bool is_dir, uint64_t size, int64_t mtime);

// Restart the active pane's search with current query/mode/case/filters.
void restart_active_search(AppState& app);

// ── Sort dropdown row model (shared by draw + click handling) ──
struct SortMenuRow {
  enum class Kind {
    Field,               // sort_field selector (field = SortField as int)
    ToggleDescending,    // reverse order
    ToggleFoldersFirst,
    ToggleHiddenLast,
    ToggleNatural,
    ToggleCaseSensitive,
    GroupCaption,        // "Group By" section caption
    GroupField,          // group_by selector (field = 0..4)
    Separator,
  };
  Kind kind = Kind::Separator;
  const char* label = "";
  int field = 0; // valid when kind == Field or GroupField
};

int sort_menu_row_count();
const SortMenuRow& sort_menu_row(int index);
inline constexpr int kSortMenuItemH = 30;
inline constexpr int kSortMenuPad = 6;

/// Re-apply the current search filter / hidden filter to the existing entries.
void apply_filter(AppState& app);
/// Re-run filtering after a filter index change.
void trigger_search_on_filter_change(AppState& app);
/// Reset all search filters to default (no filtering).
void reset_search_filters(AppState& app);
/// Safely reset any preview state (destroys thumbnail ref).
void reset_preview(AppState& app);
/// Check if hover preview should activate (called each loop iteration).
void check_hover_preview(AppState& app);
/// Hide the rich tooltip popup and clear its timer state.
void hide_tooltip(AppState& app);
/// Check if the rich tooltip should activate (called each loop iteration).
void check_hover_tooltip(AppState& app);
/// Toggle space/enter preview for the selected file.
void toggle_space_preview(AppState& app);
/// Activate (or switch) space preview — does not toggle off.
void activate_space_preview(AppState& app);

/// Create a new tab (clones current navigation state).
void new_tab(AppState& app);

/// Close the active tab. If only one tab remains, does nothing.
void close_tab(AppState& app);

/// Switch to the next / previous tab.
void next_tab(AppState& app);
void prev_tab(AppState& app);

/// Clear the thumbnail cache.
void clear_thumb_cache(AppState& app);

/// Decode one queued thumbnail and schedule a redraw.
/// Returns false when the queue is empty.
bool process_pending_thumbnails(AppState& app);

/// Refresh sidebar locations (home dirs, drives, etc.).
void refresh_sidebar(AppState& app);

/// Refresh computer view items (user dirs, drives, network).
void refresh_computer(AppState& app);

/// Mount a drive from the sidebar via UDisks2. Sets mount_result for the event loop.
void mount_drive(AppState& app, int sb_idx);

/// Unmount a drive from the sidebar via UDisks2. Sets unmount_result for the event loop.
void unmount_drive(AppState& app, int sb_idx);

/// Open the selected file(s) with the default application.
void open_selected(AppState& app);

/// Open a context menu for the given item index.
void open_context_menu(AppState& app, int item_idx, int x, int y);

// ── drawing helpers (file_browser_draw.cpp) ──────────────────────

/// Pre-initialise fontconfig/Pango so the first popup/tooltip doesn't stall.
void warmup_text_rendering();

void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h,
                       double r);
void draw_scrollbar(AppState& app, cairo_t* cr, int x, int y, int h,
                    int content_h, int view_h, int scroll_px, double r,
                    double g, double b, bool computer_view = false);
cairo_surface_t* get_thumbnail(AppState& app, const std::string& path,
                                int size);
void draw_sidebar(AppState& app, cairo_t* cr, int sidebar_w, int top_y,
                  int view_h);
void draw_top_bar(AppState& app, cairo_t* cr, int w, int top_h, int pane_x = 0, int pane_w = 0);
void draw_tab_bar(AppState& app, cairo_t* cr, int w, int tab_h, int pane_x = 0, int pane_w = 0);
void draw_list_view(AppState& app, cairo_t* cr, int content_x, int content_y,
                    int content_w, int view_h);
void draw_grid_view(AppState& app, cairo_t* cr, int content_x, int content_y,
                    int content_w, int view_h);
void draw_computer_view(AppState& app, cairo_t* cr, int content_x, int content_y,
                        int content_w, int view_h);
void draw_tree_view(AppState& app, cairo_t* cr, int content_x, int content_y,
                    int content_w, int view_h);
void draw_compact_view(AppState& app, cairo_t* cr, int content_x, int content_y,
                       int content_w, int view_h);
void draw_status_bar(AppState& app, cairo_t* cr, int w, int h, int status_h);
void draw_select_dir_bar(AppState& app, cairo_t* cr, int w, int h, int bar_h);
void draw_create_dialog(AppState& app, cairo_t* cr);
void draw_rename_ui(AppState& app, cairo_t* cr);
void draw_batch_rename(AppState& app, cairo_t* cr);
void draw_confirm_dialog(AppState& app, cairo_t* cr);
void draw_conflict_dialog(AppState& app, cairo_t* cr);
void draw_password_dialog(AppState& app, cairo_t* cr);
void draw_compress_dialog(AppState& app, cairo_t* cr);
void draw_terminal_chooser(AppState& app, cairo_t* cr);
void draw_context_menu(AppState& app, cairo_t* cr);
void draw_marquee(AppState& app, cairo_t* cr);

// ── hit testing (file_browser_draw.cpp) ──────────────────────────

int hit_test_list(AppState& app, int x, int y);
int hit_test_grid(AppState& app, int x, int y);
int hit_test_computer(AppState& app, int x, int y);
int hit_test_tree(AppState& app, int x, int y, bool for_click = false);
int hit_test_compact(AppState& app, int x, int y);
void build_tree_entries(AppState& app);
int hit_test_sidebar(AppState& app, int x, int y);
int hit_test_context_menu(AppState& app, int x, int y);

/// Returns true if (x, y) is within the Favorites section area of the sidebar
/// (not on an individual item, but anywhere in the section).
bool hit_test_fav_section(AppState& app, int x, int y);
void hit_test_marquee(AppState& app);

// ── context menu logic (file_browser_menu.cpp) ───────────────────

void execute_context_menu_action(AppState& app, int item_idx);

// ── Open With dialog (file_browser_menu.cpp + draw.cpp) ──────────

void open_with_open(AppState& app, const std::string& file_path);
void open_with_close(AppState& app);
void draw_open_with(AppState& app, cairo_t* cr);

// ── Settings dialog (file_browser_menu.cpp + draw.cpp) ───────────

void open_settings(AppState& app);
void save_file_browser_settings(AppState& app);
void request_fs_operation(AppState& app, const std::vector<std::string>& srcs,
                          const std::string& dest_dir, bool is_move,
                          const std::string& success_toast, bool clear_cut = false);
// Paste clipboard contents into dest_dir (or the current directory when
// empty): file URIs if present, otherwise saves clipboard image data
// (screenshots) as a new image file.
void paste_clipboard(AppState& app, const std::string& dest_dir = {});
void resolve_conflict_choice(AppState& app, int choice);  // 0=Overwrite/Merge, 1=Skip, 2=Cancel
void settings_apply(AppState& app);
void draw_settings_dialog(AppState& app, cairo_t* cr);
void draw_sort_menu(AppState& app, cairo_t* cr);
void draw_columns_menu(AppState& app, cairo_t* cr);
void draw_search_banner(AppState& app, cairo_t* cr, int x, int y, int w);
void draw_filter_dropdown(AppState& app, cairo_t* cr, int section);
void draw_hover_preview(AppState& app, cairo_t* cr);
void draw_properties_dialog(AppState& app, cairo_t* cr);
void draw_info_panel(AppState& app, cairo_t* cr);
void draw_operations_panel(AppState& app, cairo_t* cr);
int properties_hit_test(AppState& app, int x, int y);
int settings_hit_test(AppState& app, int x, int y);
void show_properties(AppState& app, const std::string& path, const std::string& icon_name = "");
void show_properties_multi(AppState& app, const std::vector<std::string>& paths);
void reload_settings_from_config(AppState& app);
void reload_colors_from_config(AppState& app);

// ── Properties window (separate xdg-toplevel) ───────────────────
void create_props_window(AppState& app);
void destroy_props_window(AppState& app);
void draw_props_window(AppState& app);
void handle_props_click(AppState& app, int x, int y, int button);

// ── Settings window (separate xdg-toplevel) ─────────────────────
void create_settings_window(AppState& app);
void destroy_settings_window(AppState& app);
void draw_settings_window(AppState& app);
void handle_settings_click(AppState& app, int x, int y, int button);

// ── compress feature (features/compress.cpp) ─────────────────────

bool is_archive_extension(const std::string& path);
std::string default_extract_dir(const std::string& archive_path);
void check_compress_tool_availability(AppState& app);
void execute_compress_async(AppState& app);
void execute_extract_async(AppState& app, const std::string& archive_path,
                            const std::string& dest_dir);

// ── terminal helpers (file_browser_terminal.cpp) ─────────────────

void scan_terminal_apps(AppState& app);
void open_terminal_at(AppState& app, const std::string& dir);

// ── shared helpers (file_browser_nav.cpp) ────────────────────────

std::string home_dir();

} // namespace eh::file_browser
