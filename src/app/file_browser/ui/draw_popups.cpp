// draw_popups.cpp — Exported from ui/draw.cpp as part of the Step 4 file split.

#include "../app.hpp"
#include "../trace.hpp"
#include "../features/sidebar.hpp"
#include "../features/view_zoom.hpp"
#include "app/file_browser/features/thumb_pool.hpp"
#include "app/file_browser/features/dir_stats.hpp"

#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "draw_helpers.hpp"
#include "draw_file_icons.hpp"
#include "draw_thumbnails.hpp"
#include "layout.hpp"

#include "platform/common/icon_cache/icon_cache.hpp"
#include "app/file_browser/features/svg_preview.hpp"
#include "app/file_browser/features/video_preview.hpp"
#include "app/file_browser/features/pdf_preview.hpp"
#include "app/file_browser/features/epub_preview.hpp"
#include "app/file_browser/features/image_preview.hpp"
#include "app/file_browser/features/thumbnail_cache.hpp"
#include "app/file_browser/ui/pixblit.hpp"

namespace fs = std::filesystem;

namespace eh::file_browser {


// ── filter label arrays ──────────────────────────────────────────

static constexpr const char* kFilterTypeLabels[] = {
  "All", "Folder", "Image", "Audio", "Video", "Text", "Document",
  "Archive", "Code", "Executable", "Web", "Font", "Markdown",
};

static constexpr const char* kFilterSizeLabels[] = {
  "Any", "< 10 KB", "10-100 KB", "100 KB-1 MB", "1-10 MB", "10-100 MB", "> 100 MB",
};

static constexpr const char* kFilterDateLabels[] = {
  "Any", "Today", "This week", "This month", "This year",
};

static constexpr const char* kFilterTypeShort[] = {
  "All", "Folder", "Img", "Aud", "Vid", "Text", "Doc",
  "Arch", "Code", "Exec", "Web", "Font", "MD",
};

static constexpr const char* kFilterSizeShort[] = {
  "Any", "<10K", "10-100K", "100K-1M", "1-10M", "10-100M", ">100M",
};

static constexpr const char* kFilterDateShort[] = {
  "Any", "Today", "Week", "Month", "Year",
};


// ── filter dropdown with expandable sections ─────────────────────

static constexpr int kFilterHdrH = 28;
static constexpr int kFilterItemH = 24;
static constexpr int kFilterPD = 6;
static constexpr int kFilterSep = 4;
static constexpr int kFilterW = 180;

void draw_filter_dropdown(AppState& app, cairo_t* cr, int section) {
  int top_h = app.top_bar_height;

  auto items_for = [](int s) { return s == 1 ? 13 : s == 2 ? 7 : 5; };
  int h = kFilterPD;
  for (int s = 1; s <= 3; ++s) {
    h += kFilterHdrH + kFilterSep;
    if (section == s) h += items_for(s) * kFilterItemH;
  }
  h += kFilterPD;

  int menu_x = app.filter_btn_x + app.filter_btn_w - kFilterW;
  if (menu_x < 0) menu_x = 0;
  int menu_y = top_h;

  (app.active_pane ? app.r_filter_dropdown_x : app.filter_dropdown_x) = menu_x;
  (app.active_pane ? app.r_filter_dropdown_y : app.filter_dropdown_y) = menu_y;
  (app.active_pane ? app.r_filter_dropdown_w : app.filter_dropdown_w) = kFilterW;
  (app.active_pane ? app.r_filter_dropdown_h : app.filter_dropdown_h) = h;

  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, menu_x + s * 2, menu_y + s * 2, kFilterW, h, 6);
    cairo_fill(cr);
  }
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, menu_x, menu_y, kFilterW, h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, menu_x + 0.5, menu_y + 0.5, kFilterW - 1, h - 1, 5.5);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);

  struct SecInfo { const char* name; const char* const* labels; int count; int cur_idx; };
  SecInfo sections[3] = {
    {"Type", kFilterTypeLabels, 13, app.active_pane ? app.r_filter_type_idx : app.filter_type_idx},
    {"Size", kFilterSizeLabels, 7, app.active_pane ? app.r_filter_size_idx : app.filter_size_idx},
    {"Date", kFilterDateLabels, 5, app.active_pane ? app.r_filter_date_idx : app.filter_date_idx},
  };

  int y = menu_y + kFilterPD;
  int glob_idx = 0;
  for (int si = 0; si < 3; ++si) {
    int sec_num = si + 1;
    bool expanded = (section == sec_num);
    auto& info = sections[si];

    // Header
      bool hdr_hit = (glob_idx == (app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover));
    if (hdr_hit) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, menu_x + 4, y, kFilterW - 8, kFilterHdrH, 4);
      cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.7);
    cairo_move_to(cr, menu_x + 10, y + kFilterHdrH / 2 + 4);
    cairo_show_text(cr, expanded ? "\u25BC " : "\u25B6 ");
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
    cairo_show_text(cr, info.name);
    if (!expanded && info.cur_idx > 0 && info.cur_idx < info.count) {
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g, app.text_secondary_b, 0.6);
      cairo_move_to(cr, menu_x + kFilterW - 12 - kFilterItemH, y + kFilterHdrH / 2 + 4);
      cairo_show_text(cr, info.labels[info.cur_idx]);
    }
    ++glob_idx;
    y += kFilterHdrH;

    // Items
    if (expanded) {
      for (int i = 0; i < info.count; ++i) {
        bool ihover = (glob_idx == (app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover));
        bool iactive = (i == info.cur_idx);
        if (ihover) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
          draw_rounded_rect(cr, menu_x + 8, y, kFilterW - 16, kFilterItemH, 4);
          cairo_fill(cr);
        }
        int tx = menu_x + 16;
        if (iactive) {
          cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
          cairo_move_to(cr, tx, y + kFilterItemH / 2 + 4);
          cairo_show_text(cr, "✓");
          tx += 14;
        }
        cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, iactive ? 1.0 : 0.85);
        cairo_move_to(cr, tx + 4, y + kFilterItemH / 2 + 4);
        cairo_show_text(cr, info.labels[i]);
        ++glob_idx;
        y += kFilterItemH;
      }
    }

    y += kFilterSep;
  }
}

// ── hover preview popup ──────────────────────────────────────────

static bool is_pdf_preview(const AppState& app) {
  return is_pdf_extension(app.preview_path);
}

// Rich tooltip metadata card (folders / entries without a live preview).
// Drawn into the tooltip subsurface at (tooltip_x, tooltip_y).
void draw_tooltip_card(AppState& app, cairo_t* cr) {
  int px = app.tooltip_x;
  int py = app.tooltip_y;
  int pw = app.tooltip_w;
  int ph = app.tooltip_h;
  int radius = 8;

  // Frame (same style as the hover preview popup)
  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, px + s * 2, py + s * 2, pw, ph, radius);
    cairo_fill(cr);
  }
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, px, py, pw, ph, radius);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, radius - 0.5);
  cairo_stroke(cr);

  int pad = 12;
  int y = py + pad + 6;

  // Title
  std::string title = app.tooltip_title;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 12);
  cairo_text_extents_t te;
  cairo_text_extents(cr, title.c_str(), &te);
  if (te.width > pw - pad * 2) {
    while (title.size() > 4 &&
           (cairo_text_extents(cr, (title + "...").c_str(), &te), te.width > pw - pad * 2)) {
      title.pop_back();
    }
    title += "...";
    cairo_text_extents(cr, title.c_str(), &te);
  }
  cairo_set_source_rgba(cr, app.text_r * 0.9, app.text_g * 0.9, app.text_b * 0.9, 0.95);
  cairo_move_to(cr, px + pad, y);
  cairo_show_text(cr, title.c_str());
  y += 24;

  // Metadata rows
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11);
  for (const auto& row : app.tooltip_rows) {
    std::string line = row;
    cairo_text_extents(cr, line.c_str(), &te);
    if (te.width > pw - pad * 2) {
      while (line.size() > 4 &&
             (cairo_text_extents(cr, (line + "...").c_str(), &te), te.width > pw - pad * 2)) {
        line.pop_back();
      }
      line += "...";
    }
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 0.95);
    cairo_move_to(cr, px + pad, y);
    cairo_show_text(cr, line.c_str());
    y += 20;
  }
}

// One-time warm-up of the cairo "toy" font stack (fontconfig init + font
// load for the families popups/tooltips use) and Pango, which otherwise
// cost 500-700ms on their first real use and stall the frame that happens
// to trigger them. Call once at startup.
void warmup_text_rendering() {
  cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
  cairo_t* cr = cairo_create(s);
  const char* fams[] = {"Sans", "monospace"};
  for (const char* fam : fams) {
    for (int weight : {CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_WEIGHT_BOLD}) {
      cairo_select_font_face(cr, fam, CAIRO_FONT_SLANT_NORMAL,
                             static_cast<cairo_font_weight_t>(weight));
      cairo_set_font_size(cr, 12);
      cairo_text_extents_t te;
      cairo_text_extents(cr, "Ag", &te);
      cairo_move_to(cr, 2, 10);
      cairo_show_text(cr, "Ag");
    }
  }
  // Sidebar/labels render via the toy API at these sizes; warming the glyph
  // caches here keeps the cost off the first real paint (~2-3 ms saved).
  {
    static const char* kUiStrings[] = {
        "My Computer", "Home", "Desktop", "Documents", "Downloads",
        "Pictures", "Music", "Videos", "Trash", "Root", "File System",
    };
    // Sidebar/toolbar render at zoom-scaled sizes (10..16 px typical); warm
    // every candidate size so the real paint never rasterizes glyphs cold.
    for (int weight : {CAIRO_FONT_WEIGHT_NORMAL, CAIRO_FONT_WEIGHT_BOLD}) {
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             static_cast<cairo_font_weight_t>(weight));
      for (double sz : {11.0, 13.0, 14.0, 16.0}) {
        cairo_set_font_size(cr, sz);
        cairo_move_to(cr, 0, sz);
        for (const char* str : kUiStrings) cairo_show_text(cr, str);
      }
    }
  }
  cairo_surface_flush(s);
  cairo_destroy(cr);
  cairo_surface_destroy(s);

#if defined(HAVE_PANGO)
  cairo_surface_t* ps = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
  cairo_t* pcr = cairo_create(ps);
  if (PangoLayout* pl = pango_cairo_create_layout(pcr)) {
    PangoFontDescription* d = pango_font_description_new();
    pango_font_description_set_family(d, "Sans");
    pango_font_description_set_absolute_size(d, 13 * PANGO_SCALE);
    pango_layout_set_font_description(pl, d);
    pango_layout_set_text(pl, "Ag", -1);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(pl, &w, &h);
    pango_cairo_show_layout(pcr, pl);
    pango_font_description_free(d);
    g_object_unref(pl);
  }
   cairo_surface_flush(ps);
   cairo_destroy(pcr);
   cairo_surface_destroy(ps);
 #endif
}

// Truncate s to fit avail_w, cutting on CHARACTER boundaries and appending
// an ellipsis. Byte-level slicing can splice multi-byte UTF-8 sequences
// (the ellipsis alone is 3 bytes); cairo flags invalid UTF-8 as
// CAIRO_STATUS_INVALID_STRING and every subsequent draw on that context
// becomes a silent no-op — which blanked everything below the first wide
// line in text previews.
static std::string utf8_clip_to_width(cairo_t* cr, const std::string& s,
                                      double avail_w, const char* ell = "\u2026") {
  cairo_text_extents_t te;
  cairo_text_extents(cr, s.c_str(), &te);
  if (s.empty() || te.width <= avail_w) return s;

  // Character start offsets.
  std::vector<size_t> b;
  b.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    b.push_back(i);
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int len = c < 0x80            ? 1
              : (c & 0xE0) == 0xC0 ? 2
              : (c & 0xF0) == 0xE0 ? 3
              : (c & 0xF8) == 0xF0 ? 4
                                   : 1;
    i += static_cast<size_t>(len);
  }

  size_t lo = 0, hi = b.size();
  while (lo < hi) {
    size_t m = (lo + hi) / 2;
    cairo_text_extents(cr, s.substr(b[m]).c_str(), &te);
    if (te.width <= avail_w) lo = m + 1;
    else hi = m;
  }
  size_t k = lo > 0 ? lo - 1 : 0;
  std::string out = s.substr(b[k]) + ell;
  cairo_text_extents(cr, out.c_str(), &te);
  while (k > 0 && te.width > avail_w) {
    --k;
    out = s.substr(b[k]) + ell;
    cairo_text_extents(cr, out.c_str(), &te);
  }
  return out;
}

void draw_hover_preview(AppState& app, cairo_t* cr) {
  int px = app.preview_x;
  int py = app.preview_y;
  int pw = app.preview_w;
  int ph = app.preview_h;
  int radius = 8;

  bool pdf = is_pdf_preview(app);
  bool space_mode = (app.preview_mode == AppState::PreviewMode::Space);

  // Entry lookup (before background so image_preview is known)
  int vi = app.preview_entry_idx;
  std::string name;
  std::string info;
  FileType type = FileType::File;
  uint64_t file_size = 0;
  int64_t file_mtime = 0;
  if (vi >= 0 && vi < static_cast<int>(app.cur_tab().visible_entries.size())) {
    int ri = app.cur_tab().visible_entries[vi];
    if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
      const auto& entry = app.cur_tab().entries[ri];
      name = entry.name;
      type = entry.type;
      file_size = entry.size;
      file_mtime = entry.modified_sec;
      auto fmt_size = [](uint64_t bytes) -> std::string {
        if (bytes < 1024) return std::to_string(bytes) + " B";
        if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
        if (bytes < 1024 * 1024 * 1024) return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
        return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
      };
      info = fmt_size(entry.size);
      const char* type_names[] = {"Folder", "Image", "Audio", "Video", "Text", "Markdown",
                                   "Code", "Document", "Font", "Archive", "Executable", "Web", "File"};
      int ti = static_cast<int>(type);
      if (ti >= 0 && ti < 13) {
        info += "  \u00B7  ";
        info += type_names[ti];
      }
    }
  }

  // ── Text preview: bespoke opaque card ────────────────────────────
  // Structured header/body/footer bands, every surface and glyph drawn at
  // full opacity — no translucency, no shadows. Renders completely and
  // returns so the generic translucent pipeline never touches text files.
  if (!app.preview_text.empty() && !pdf) {
    auto mixc = [](double a, double b, double t) {
      return a + (b - a) * t;
    };
    const double hdr_bg[3] = {mixc(app.surface_r, app.text_r, 0.08),
                              mixc(app.surface_g, app.text_g, 0.08),
                              mixc(app.surface_b, app.text_b, 0.08)};
    const double ftr_bg[3] = {mixc(app.surface_r, app.text_r, 0.05),
                              mixc(app.surface_g, app.text_g, 0.05),
                              mixc(app.surface_b, app.text_b, 0.05)};
    constexpr int kHeaderH = 34, kFooterH = 26;
    const int body_top = py + kHeaderH;
    const int body_bot = py + ph - kFooterH;

    // Card: opaque fill + crisp border.
    double tr, tg, tb;
    wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
    cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
    draw_rounded_rect(cr, px, py, pw, ph, radius);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 1.0);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, radius - 0.5);
    cairo_stroke(cr);

    // Clip everything inside the rounded card while drawing bands/content.
    cairo_save(cr);
    draw_rounded_rect(cr, px, py, pw, ph, radius);
    cairo_clip(cr);

    // Header band.
    cairo_set_source_rgb(cr, hdr_bg[0], hdr_bg[1], hdr_bg[2]);
    cairo_rectangle(cr, px, py, pw, kHeaderH);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 1.0);
    cairo_rectangle(cr, px, py + kHeaderH - 1, pw, 1);
    cairo_fill(cr);

    cairo_text_extents_t te;
    const double side_pad = 12.0;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);

    // Size tag on the right (fixed content, measure first).
    auto fmt_sz = [](uint64_t b) -> std::string {
      char buf[32];
      if (b < 1024) snprintf(buf, sizeof buf, "%u B", (unsigned)b);
      else if (b < 1024ull * 1024) snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0);
      else snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024.0));
      return buf;
    };
    const std::string size_tag = fmt_sz(file_size);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_text_extents(cr, size_tag.c_str(), &te);
    const double size_w = te.width;
    cairo_move_to(cr, px + pw - side_pad - size_w, py + kHeaderH / 2.0 + 4);
    cairo_show_text(cr, size_tag.c_str());

    // Filename, pixel-truncated between padding and the size tag.
    const double name_avail = pw - side_pad * 2 - size_w - 14.0;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    const std::string shown = utf8_clip_to_width(cr, name, name_avail);
    cairo_move_to(cr, px + side_pad, py + kHeaderH / 2.0 + 4);
    cairo_show_text(cr, shown.c_str());

    // Body: normalize text exactly like the reader wants to see it.
    // Subtitle formats (SubRip/WebVTT): drop cue sequence numbers and
    // timestamp lines so the preview shows the actual dialogue, and
    // collapse the blank runs between cues into single separators.
    std::string src_text = app.preview_text;
    bool is_subtitle = false;
    {
      std::string lower_name = name;
      for (auto& c : lower_name) c = static_cast<char>(std::tolower((unsigned char)c));
      if (lower_name.size() > 4) {
        // Last four bytes: ".srt", ".vtt", ".ass", ".ssa", ".sub".
        const std::string e4 = lower_name.substr(lower_name.size() - 4);
        is_subtitle = e4 == ".srt" || e4 == ".vtt" || e4 == ".ass" ||
                      e4 == ".ssa" || e4 == ".sub";
      }
    }
    if (is_subtitle) {
      std::string cleaned;
      cleaned.reserve(src_text.size());
      bool last_blank = true;   // swallow leading blanks
      size_t p = 0;
      while (p < src_text.size()) {
        size_t nl = src_text.find('\n', p);
        std::string ln = (nl == std::string::npos)
                             ? src_text.substr(p)
                             : src_text.substr(p, nl - p);
        p = (nl == std::string::npos) ? src_text.size() : nl + 1;
        // strip BOM/CR
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (!ln.empty() && ln.front() == '\xEF') {
          // UTF-8 BOM on first line
          size_t skip = (ln.size() >= 3 &&
                         (unsigned char)ln[0] == 0xEF &&
                         (unsigned char)ln[1] == 0xBB &&
                         (unsigned char)ln[2] == 0xBF) ? 3 : 0;
          ln.erase(0, skip);
        }
        bool ts = ln.find("-->") != std::string::npos;
        bool digits_only = !ln.empty() &&
                           ln.find_first_not_of("0123456789 \t") == std::string::npos;
        bool blank_like = ln.find_first_not_of(" \t") == std::string::npos;
        if (blank_like) {
          if (!last_blank && !cleaned.empty()) {
            cleaned += '\n';
            last_blank = true;
          }
          continue;
        }
        if (ts || digits_only) continue;   // cue metadata
        // Strip inline markup (<b>, <i>, <u>, <font …>) — SubRip/WebVTT
        // allow basic HTML-ish tags; raw them reads as noise in a preview.
        std::string plain;
        plain.reserve(ln.size());
        for (size_t q = 0; q < ln.size();) {
          if (ln[q] == '<') {
            size_t close = ln.find('>', q);
            if (close == std::string::npos) break;   // stray '<' → drop rest
            q = close + 1;
            continue;
          }
          plain += ln[q++];
        }
        if (plain.find_first_not_of(" \t") == std::string::npos) continue;
        cleaned += plain;
        cleaned += '\n';
        last_blank = false;
      }
      src_text.swap(cleaned);
    }

    std::string norm;
    norm.reserve(src_text.size());
    {
      int col = 0;
      for (char ch : src_text) {
        if (ch == '\t') { do { norm += ' '; } while ((++col) % 4); continue; }
        if (ch == '\r') continue;
        if (static_cast<unsigned char>(ch) < 32 && ch != '\n') continue;
        if (ch == '\n') col = 0; else ++col;
        norm += ch;
      }
    }
    long total_lines = 1;
    for (char ch : norm) if (ch == '\n') ++total_lines;

    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    const double pad = 14.0;
    const double avail_w = pw - pad * 2.0;
    const double line_h = 16.0;
    int lines_max = static_cast<int>((body_bot - body_top - 8) / line_h);
    size_t pos = norm.find_first_not_of(" \n");
    if (pos == std::string::npos) pos = norm.size();

    // Word-wrap like a book page: each logical line flows across as many
    // display rows as the card width allows; words stay intact (a single
    // over-long word hard-breaks at the character boundary that fits).
    auto wrap_line = [&](const std::string& s,
                         std::vector<std::string>& out) {
      if (s.empty()) { out.emplace_back(); return; }
      // b[i] = byte offset where character i starts; b.back() = s.size().
      // All loop state below is a CHARACTER INDEX into b — mixing indices
      // with byte offsets here used to let seg_lo walk backwards on
      // multibyte lines and spin the paint thread forever.
      std::vector<size_t> b;
      b.reserve(s.size());
      for (size_t i = 0; i < s.size();) {
        b.push_back(i);
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80            ? 1
                  : (c & 0xE0) == 0xC0 ? 2
                  : (c & 0xF0) == 0xE0 ? 3
                  : (c & 0xF8) == 0xF0 ? 4
                                       : 1;
        i += static_cast<size_t>(len);
      }
      b.push_back(s.size());
      const size_t last = b.size() - 1;   // sentinel index

      cairo_text_extents_t wte;
      size_t from = 0;
      size_t guard = 0;
      while (from < last && ++guard <= 4096) {
        // Largest character index whose span from `from` fits the width.
        size_t lo = from + 1, hi = last, fit = from + 1;
        while (lo <= hi) {
          const size_t mid = (lo + hi) / 2;
          cairo_text_extents(cr, s.substr(b[from], b[mid] - b[from]).c_str(), &wte);
          if (wte.width <= avail_w) { fit = mid; lo = mid + 1; }
          else hi = mid - 1;
        }
        // Prefer breaking after the last space inside the fit window.
        if (fit < last) {
          size_t cut = fit;
          while (cut > from + 1 && s[b[cut] - 1] != ' ') --cut;
          if (cut > from + 1) fit = cut;
        }
        out.push_back(s.substr(b[from], b[fit] - b[from]));
        from = fit;
        if (from < last && s[b[from]] == ' ') ++from;   // rows never start indented
      }
    };

    double ty = body_top + 14;
    int shown_lines = 0;
    bool content_remains = false;
    {
      std::vector<std::string> rows;
      size_t lp = pos;
      while (lp < norm.size() &&
             static_cast<int>(rows.size()) < lines_max) {
        size_t nl = norm.find('\n', lp);
        std::string logical =
            (nl == std::string::npos) ? norm.substr(lp) : norm.substr(lp, nl - lp);
        lp = (nl == std::string::npos) ? norm.size() : nl + 1;
        wrap_line(logical, rows);
      }
      if (lp < norm.size()) content_remains = true;   // stopped mid-file
      if (static_cast<int>(rows.size()) > lines_max) {
        rows.resize(lines_max);
        content_remains = true;
      }
      for (const auto& r : rows) {
        cairo_move_to(cr, px + pad, ty);
        cairo_show_text(cr, r.c_str());
        ty += line_h;
      }
      shown_lines = static_cast<int>(rows.size());
      if (lp >= norm.size() && !norm.empty() && norm.back() != '\n')
        content_remains = content_remains || shown_lines == lines_max;
    }

    // Footer band: type · lines.
    cairo_set_source_rgb(cr, ftr_bg[0], ftr_bg[1], ftr_bg[2]);
    cairo_rectangle(cr, px, body_bot, pw, kFooterH);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 1.0);
    cairo_rectangle(cr, px, body_bot, pw, 1);
    cairo_fill(cr);
    const char* type_name =
        is_subtitle ? "SUBTITLES" :
        type == FileType::Markdown ? "MARKDOWN" :
        type == FileType::Code ? "CODE" : "TEXT";
    char meta[96];
    if (content_remains)
      snprintf(meta, sizeof meta, "%s \u00B7 %d+ lines", type_name, shown_lines);
    else
      snprintf(meta, sizeof meta, "%s \u00B7 %ld line%s", type_name,
               total_lines, total_lines == 1 ? "" : "s");
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10.5);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_text_extents(cr, meta, &te);
    cairo_move_to(cr, px + side_pad, body_bot + kFooterH / 2.0 + 3.5);
    cairo_show_text(cr, meta);
    // Right side: last-modified date.
    {
      char dbuf[64] = "";
      if (file_mtime > 0) {
        struct tm lt{};
        time_t mt = static_cast<time_t>(file_mtime);
        localtime_r(&mt, &lt);
        strftime(dbuf, sizeof dbuf, "%b %e, %Y", &lt);
      }
      if (dbuf[0]) {
        cairo_text_extents(cr, dbuf, &te);
        cairo_move_to(cr, px + pw - side_pad - te.width,
                      body_bot + kFooterH / 2.0 + 3.5);
        cairo_show_text(cr, dbuf);
      }
    }
    cairo_restore(cr);   // card clip
    return;
  }

  // ── Media preview (image / video): opaque framed card ────────────
  // The image is the hero: it backs the whole body (cover-fit, darkened)
  // so letterboxing shows a dimmed extension of itself instead of empty
  // chrome; the full frame sits centered on top. Videos get a play badge.
  // While the async decode is in flight (or if it failed) the same card
  // renders a branded placeholder — a preview ALWAYS appears on hover.
  // Hover mode only: the Space full-screen preview keeps its own layout.
  if (!space_mode && (type == FileType::Image || type == FileType::Video)) {
    auto mixc = [](double a, double b, double t) { return a + (b - a) * t; };
    const double ftr_bg[3] = {mixc(app.surface_r, app.text_r, 0.05),
                              mixc(app.surface_g, app.text_g, 0.05),
                              mixc(app.surface_b, app.text_b, 0.05)};
    constexpr int kFooterH = 26;
    const int body_top = py;
    const int body_h = ph - kFooterH;
    const int body_bot = py + body_h;

    // Opaque card base + crisp border.
    double tr, tg, tb;
    wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
    cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
    draw_rounded_rect(cr, px, py, pw, ph, radius);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 1.0);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, radius - 0.5);
    cairo_stroke(cr);

    cairo_save(cr);
    draw_rounded_rect(cr, px, py, pw, ph, radius);
    cairo_clip(cr);

    int tw = 0, th = 0;
    if (app.preview_thumb) {
      tw = cairo_image_surface_get_width(app.preview_thumb);
      th = cairo_image_surface_get_height(app.preview_thumb);
    }

    if (app.preview_thumb && tw > 0 && th > 0) {
      // 1) Cover-fit backdrop: the image itself fills the body, cropped.
      {
        const double cw = static_cast<double>(pw) / tw;
        const double chh = static_cast<double>(body_h) / th;
        const double cover = std::max(cw, chh);
        const int dw = static_cast<int>(tw * cover + 0.5);
        const int dh = static_cast<int>(th * cover + 0.5);
        cairo_save(cr);
        cairo_rectangle(cr, px, py, pw, body_h);
        cairo_clip(cr);
        cairo_translate(cr, px + (pw - dw) / 2, py + (body_h - dh) / 2);
        cairo_scale(cr, cover, cover);
        cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
        cairo_paint(cr);
        // Dark scrim so the contained frame pops (painted over the opaque
        // backdrop — the card itself stays fully opaque).
        cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
        cairo_paint(cr);
        cairo_restore(cr);
      }

      // 2) The frame, CONTAIN: scales to show the WHOLE frame (never crops).
      //    Because the popup body aspect is derived from the frame, contain
      //    fills the body edge-to-edge; any sub-pixel/minsize letterbox
      //    shows the darkened cover extension behind it, not empty chrome.
      {
        const int avail_w = pw;
        const int avail_h = body_h;
        const double contain = std::min(static_cast<double>(avail_w) / tw,
                                        static_cast<double>(avail_h) / th);
        const int dw = std::max(1, static_cast<int>(tw * contain));
        const int dh = std::max(1, static_cast<int>(th * contain));
        const int dx = px + (pw - dw) / 2;
        const int dy = py + (body_h - dh) / 2;
        cairo_save(cr);
        cairo_rectangle(cr, px, py, pw, body_h);
        cairo_clip(cr);
        cairo_translate(cr, dx, dy);
        cairo_scale(cr, contain, contain);
        cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);

        // 3) Video: play badge centered on the frame.
        if (type == FileType::Video) {
          const double cx = dx + dw / 2.0, cy = dy + dh / 2.0;
          const double r = std::min(dw, dh) * 0.16 + 10.0;
          cairo_set_source_rgba(cr, 0, 0, 0, 0.72);
          cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
          cairo_fill(cr);
          cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
          cairo_set_line_width(cr, 1.5);
          cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
          cairo_stroke(cr);
          const double tr = r * 0.52;
          cairo_move_to(cr, cx - tr * 0.62, cy - tr);
          cairo_line_to(cr, cx - tr * 0.62, cy + tr);
          cairo_line_to(cr, cx + tr * 0.85, cy);
          cairo_close_path(cr);
          cairo_fill(cr);
        }
      }
    } else {
      // Placeholder body: branded, instant, upgraded when decode lands.
      cairo_set_source_rgba(cr, mixc(app.surface_r, app.text_r, 0.04),
                            mixc(app.surface_g, app.text_g, 0.04),
                            mixc(app.surface_b, app.text_b, 0.04), 1.0);
      cairo_rectangle(cr, px, py, pw, body_h);
      cairo_fill(cr);
      const auto* glyph = app.icons.tray_icon(type == FileType::Image
                                                  ? "image-x-generic"
                                                  : "video-x-generic", 64);
      if (glyph && glyph->surface) {
        const double gs = 56.0;
        const double sc = gs / std::max(1, std::max(glyph->width, glyph->height));
        cairo_save(cr);
        cairo_translate(cr, px + (pw - gs) / 2.0, py + (body_h - gs) / 2.0 - 10);
        cairo_scale(cr, sc, sc);
        cairo_set_source_surface(cr, glyph->surface, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
      }
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 11);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                            app.text_secondary_b, 1.0);
      cairo_text_extents_t pte;
      const char* msg = type == FileType::Video ? "Extracting frame\u2026"
                                                : "Loading preview\u2026";
      cairo_text_extents(cr, msg, &pte);
      cairo_move_to(cr, px + (pw - pte.width) / 2.0,
                    py + body_h / 2.0 + 26.0);
      cairo_show_text(cr, msg);
    }

    // Footer band: name left, dimensions · size right.
    cairo_set_source_rgb(cr, ftr_bg[0], ftr_bg[1], ftr_bg[2]);
    cairo_rectangle(cr, px, body_bot, pw, kFooterH);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 1.0);
    cairo_rectangle(cr, px, body_bot, pw, 1);
    cairo_fill(cr);

    auto fmt_sz = [](uint64_t b) -> std::string {
      char buf[32];
      if (b < 1024) snprintf(buf, sizeof buf, "%u B", (unsigned)b);
      else if (b < 1024ull * 1024) snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0);
      else snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024.0));
      return buf;
    };
    std::string right;
    if (tw > 0 && th > 0)
      right = std::to_string(tw) + "\u00D7" + std::to_string(th) + "  \u00B7  ";
    right += fmt_sz(file_size);

    const double side_pad = 12.0;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                          app.text_secondary_b, 1.0);
    cairo_text_extents_t mte;
    cairo_text_extents(cr, right.c_str(), &mte);
    cairo_move_to(cr, px + pw - side_pad - mte.width,
                  body_bot + kFooterH / 2.0 + 4);
    cairo_show_text(cr, right.c_str());

    const double name_avail = pw - side_pad * 2 - mte.width - 14.0;
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    const std::string shown = utf8_clip_to_width(cr, name, name_avail);
    cairo_move_to(cr, px + side_pad, body_bot + kFooterH / 2.0 + 4);
    cairo_show_text(cr, shown.c_str());

    cairo_restore(cr);   // card clip
    return;
  }

  bool fill_preview = ((type == FileType::Image || type == FileType::Video) &&
                       app.preview_thumb);

  // ── Context‑menu style popup (solid fill + outline) ──
  double frame_alpha = app.preview_opacity_pct / 100.0;
  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0) * frame_alpha;
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, px + s * 2, py + s * 2, pw, ph, radius);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b, frame_alpha);
  draw_rounded_rect(cr, px, py, pw, ph, radius);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25 * frame_alpha);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, px + 0.5, py + 0.5, pw - 1, ph - 1, radius - 0.5);
  cairo_stroke(cr);

  bool has_text = !app.preview_text.empty();
  int content_top = py + 10;
  int content_bottom = py + ph - 50;

  if (has_text) {
    // Document-peek style: top-left aligned monospace lines, pixel-accurate
    // ellipsis, tabs expanded — reads like the file instead of a jumble of
    // centered, hard-clipped strings.
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.88);
    const double pad = 14.0;
    const double avail_w = pw - pad * 2.0;
    const double line_h = 16.0;
    int lines_max = static_cast<int>((content_bottom - content_top) / line_h);

    // Normalize: expand tabs to 4-space stops, drop stray control chars,
    // strip CR from CRLF, then skip leading blank lines so the card opens
    // on real content.
    std::string norm;
    norm.reserve(app.preview_text.size());
    {
      int col = 0;
      for (char ch : app.preview_text) {
        if (ch == '\t') {
          do { norm += ' '; } while ((++col) % 4);
          continue;
        }
        if (ch == '\r') continue;
        if (static_cast<unsigned char>(ch) < 32 && ch != '\n') continue;
        if (ch == '\n') col = 0; else ++col;
        norm += ch;
      }
    }
    size_t pos = norm.find_first_not_of(" \n");
    if (pos == std::string::npos) pos = norm.size();

    double ty = content_top + 13;
    cairo_text_extents_t te;
    for (int l = 0; l < lines_max && pos < norm.size(); ++l) {
      size_t nl = norm.find('\n', pos);
      std::string line_str =
          (nl == std::string::npos) ? norm.substr(pos) : norm.substr(pos, nl - pos);
      pos = (nl == std::string::npos) ? norm.size() : nl + 1;

      // Pixel-width clip with an ellipsis (never mid-codepoint: ASCII-safe
      // trim bytewise is fine for display purposes here).
      cairo_text_extents(cr, line_str.c_str(), &te);
      if (te.width > avail_w) {
        size_t lo = 0, hi = line_str.size();
        while (lo < hi) {
          size_t mid = (lo + hi) / 2;
          cairo_text_extents(cr, line_str.substr(0, mid).c_str(), &te);
          if (te.width <= avail_w) lo = mid + 1; else hi = mid;
        }
        size_t cut = lo > 0 ? lo - 1 : 0;
        line_str = line_str.substr(0, cut) + "\u2026";
        cairo_text_extents(cr, line_str.c_str(), &te);
        while (line_str.size() > 1 && te.width > avail_w) {
          line_str.erase(line_str.size() - 2, 1);   // shed before the …
          cairo_text_extents(cr, line_str.c_str(), &te);
        }
      }

      cairo_move_to(cr, px + pad, ty);
      cairo_show_text(cr, line_str.c_str());
      ty += line_h;
    }
  } else if (app.preview_thumb) {
    int tw = cairo_image_surface_get_width(app.preview_thumb);
    int th = cairo_image_surface_get_height(app.preview_thumb);

    if (fill_preview && tw > 0 && th > 0) {
      // Full-bleed: the popup is aspect-matched to the thumbnail, so cover
      // + center fills the entire window edge-to-edge (cropping only sub-
      // pixel rounding slivers). The CTM translate/scale makes the source
      // actually grow to the rect — a plain set_source_surface would draw at
      // native size and leave the card surface visible around the image.
      int img_x = px;
      int img_y = py;
      int img_w = pw;
      int img_h = ph;
      double scale = std::max(static_cast<double>(img_w) / tw,
                              static_cast<double>(img_h) / th);
      int dw = static_cast<int>(tw * scale);
      int dh = static_cast<int>(th * scale);
      int dx = img_x + (img_w - dw) / 2;
      int dy = img_y + (img_h - dh) / 2;
      cairo_save(cr);
      draw_rounded_rect(cr, px, py, pw, ph, radius);
      cairo_clip(cr);
      cairo_rectangle(cr, dx, dy, dw, dh);
      cairo_clip(cr);
      cairo_translate(cr, dx, dy);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    } else if (pdf && tw > 0 && th > 0) {
      int pdf_margin = 8;
      int page_w = pw - pdf_margin * 2;
      int page_h = content_bottom - content_top - pdf_margin;
      double scale = std::min(static_cast<double>(page_w) / tw,
                              static_cast<double>(page_h) / th);
      int dw = static_cast<int>(tw * scale);
      int dh = static_cast<int>(th * scale);
      int dx = px + (pw - dw) / 2;
      int dy = content_top + pdf_margin + (page_h - dh) / 2;
      cairo_save(cr);
      cairo_rectangle(cr, dx, dy, dw, dh);
      cairo_set_source_rgba(cr, 1, 1, 1, 1);
      cairo_fill(cr);
      cairo_rectangle(cr, dx, dy, dw, dh);
      cairo_clip(cr);
      cairo_translate(cr, dx, dy);
      cairo_scale(cr, scale, scale);
      cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    } else if (tw > 0 && th > 0) {
      if (space_mode) {
        int img_margin = 12;
        int avail_w = pw - img_margin * 2;
        int avail_h = ph - img_margin * 2 - 50;
        double s = std::min(static_cast<double>(avail_w) / tw,
                            static_cast<double>(avail_h) / th);
        int dw = static_cast<int>(tw * s);
        int dh = static_cast<int>(th * s);
        int dx = px + (pw - dw) / 2;
        int dy = py + (ph - 50 - dh) / 2;
        cairo_save(cr);
        cairo_rectangle(cr, dx, dy, dw, dh);
        cairo_clip(cr);
        cairo_translate(cr, dx, dy);
        cairo_scale(cr, s, s);
        cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        int ts = std::min(pw - 20, content_bottom - content_top - 10);
        if (ts > 180) ts = 180;
        double scale = static_cast<double>(ts) / std::max(tw, th);
        int dw = static_cast<int>(tw * scale);
        int dh = static_cast<int>(th * scale);
        int dx = px + (pw - dw) / 2;
        int dy = content_top + (content_bottom - content_top - dh) / 2;
        cairo_save(cr);
        cairo_rectangle(cr, dx, dy, dw, dh);
        cairo_clip(cr);
        cairo_translate(cr, dx, dy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, app.preview_thumb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
      }
    }
  }

  // File name (truncated if long, keeps extension)
  if (fill_preview) {
    // Translucent band so the filename/info stay legible over the image.
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    cairo_rectangle(cr, px, py + ph - 52, pw, 52);
    cairo_fill(cr);
  }
  double color_adj = 0.9;
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12);
  // Fit the full name to the popup width with a real ellipsis, keeping the
  // extension so the type stays visible — long names must read in full.
  {
    const int avail = std::max(40, pw - 24);
    cairo_text_extents_t mte;
    cairo_text_extents(cr, name.c_str(), &mte);
    if (mte.width > avail) {
      std::string ext;
      auto dot = name.rfind('.');
      if (dot != std::string::npos && dot + 1 < name.size()) {
        ext = name.substr(dot);      // includes the '.'
        name = name.substr(0, dot);  // stem only
      }
      std::size_t k = name.size();
      bool fit = false;
      while (k > 0) {
        std::string cand = name.substr(0, k) + "..." + ext;
        cairo_text_extents(cr, cand.c_str(), &mte);
        if (mte.width <= avail) { name = cand; fit = true; break; }
        --k;
      }
      if (!fit) name = "..." + ext; // even one char + ext is too wide
    }
  }
  cairo_set_source_rgba(cr, app.text_r * color_adj, app.text_g * color_adj, app.text_b * color_adj, 0.9);
  cairo_text_extents_t te;
  cairo_text_extents(cr, name.c_str(), &te);
  double name_x = px + (pw - te.width) / 2;
  double name_y = py + ph - 36;
  cairo_move_to(cr, name_x, name_y);
  cairo_show_text(cr, name.c_str());

  // File info (size + type)
  cairo_set_font_size(cr, 11);
  cairo_set_source_rgba(cr, app.text_secondary_r * color_adj, app.text_secondary_g * color_adj, app.text_secondary_b * color_adj, 0.7);
  cairo_text_extents(cr, info.c_str(), &te);
  cairo_move_to(cr, px + (pw - te.width) / 2, name_y + 16);
  cairo_show_text(cr, info.c_str());
}

// ── search results banner ────────────────────────────────────────

void draw_search_banner(AppState& app, cairo_t* cr, int x, int y, int w) {
  constexpr int kBannerH = 28;
  double zf = app.zoom_pct / 100.0;

  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.12);
  cairo_rectangle(cr, x, y, w, kBannerH);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.35);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, x, y + kBannerH - 0.5);
  cairo_line_to(cr, x + w, y + kBannerH - 0.5);
  cairo_stroke(cr);

  bool rec = app.active_pane ? app.r_recursive_search_active
                             : app.recursive_search_active;
  const auto& q = app.active_pane ? app.r_search_query : app.search_query;
  std::string root = rec ? home_dir() : app.cur_tab().current_path;
  if (root.size() > 48) root = "…" + root.substr(root.size() - 47);

  char count_buf[64];
  std::snprintf(count_buf, sizeof(count_buf), "%d",
                static_cast<int>(app.cur_tab().visible_entries.size()));

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 12.0 * zf);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.85);
  std::string line = "Results for \u201C" + q + "\u201D in " + root +
                     " \u2014 " + count_buf + " items";
  cairo_move_to(cr, x + 12, y + kBannerH / 2 + 4);
  cairo_show_text(cr, line.c_str());

  // Clear button (right side)
  const char* clear_label = "Clear Search";
  cairo_text_extents_t te;
  cairo_text_extents(cr, clear_label, &te);
  int bw = static_cast<int>(te.width) + static_cast<int>(16.0 * zf);
  int bh = static_cast<int>(20.0 * zf);
  int bx = x + w - bw - 8;
  int by = y + (kBannerH - bh) / 2;
  app.search_banner_clear_x = bx;
  app.search_banner_clear_w = bw;

  bool hv = app.search_banner_clear_hover;
  cairo_set_source_rgba(cr, app.surface_r, app.surface_g, app.surface_b,
                        hv ? 0.9 : 0.6);
  draw_rounded_rect(cr, bx, by, bw, bh, 5);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
  draw_rounded_rect(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 5);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 0.9);
  cairo_move_to(cr, bx + 8, by + bh / 2 + 4);
  cairo_show_text(cr, clear_label);
}

// ── sort menu dropdown ───────────────────────────────────────────

static const std::vector<SortMenuRow>& sort_menu_rows() {
  using K = SortMenuRow::Kind;
  using I = SortMenuRow::Icon;
  static const std::vector<SortMenuRow> rows = {
    {K::Field, "Name", I::Hash, static_cast<int>(SortField::Name)},
    {K::Field, "Size", I::Bars, static_cast<int>(SortField::Size)},
    {K::Field, "Date Modified", I::Clock, static_cast<int>(SortField::Modified)},
    {K::Field, "Type", I::FileText, static_cast<int>(SortField::Type)},
    {K::Field, "Owner", I::Person, static_cast<int>(SortField::Owner)},
    {K::Field, "Group", I::People, static_cast<int>(SortField::Group)},
    {K::Field, "Permissions", I::Shield, static_cast<int>(SortField::Permissions)},
    {K::Field, "Extension", I::File, static_cast<int>(SortField::Extension)},
    {K::Field, "Link Target", I::Link, static_cast<int>(SortField::LinkTarget)},
    {K::Field, "First Modified", I::ArrowDownward, static_cast<int>(SortField::FirstModified)},
    {K::Field, "Last Modified", I::Clock, static_cast<int>(SortField::LastModified)},
    {K::Separator, "", I::None, 0},
    {K::ToggleDescending, "Descending", I::ArrowDownward, 0},
    {K::ToggleFoldersFirst, "Folders First", I::Folder, 0},
    {K::ToggleHiddenLast, "Hidden Last", I::EyeOff, 0},
    {K::ToggleNatural, "Natural Order", I::List, 0},
    {K::ToggleCaseSensitive, "Case Sensitive", I::Text, 0},
    {K::Separator, "", I::None, 0},
    {K::GroupCaption, "Group By", I::None, 0},
    {K::GroupField, "No Grouping", I::Minus, 0},
    {K::GroupField, "Type", I::FileText, 1},
    {K::GroupField, "Name", I::Hash, 2},
    {K::GroupField, "Date", I::Clock, 3},
    {K::GroupField, "Size", I::Bars, 4},
  };
  return rows;
}

int sort_menu_row_count() { return static_cast<int>(sort_menu_rows().size()); }

const SortMenuRow& sort_menu_row(int index) {
  return sort_menu_rows()[static_cast<size_t>(index)];
}

static cairo_surface_t* sort_menu_icon_surface(AppState& app, SortMenuRow::Icon ic) {
  switch (ic) {
    case SortMenuRow::Icon::Hash: return app.icon_hash_svg;
    case SortMenuRow::Icon::Bars: return app.icon_bars_svg;
    case SortMenuRow::Icon::Clock: return app.icon_clock_svg;
    case SortMenuRow::Icon::FileText: return app.icon_file_text_svg;
    case SortMenuRow::Icon::Person: return app.icon_person_svg;
    case SortMenuRow::Icon::People: return app.icon_people_svg;
    case SortMenuRow::Icon::Shield: return app.icon_shield_svg;
    case SortMenuRow::Icon::File: return app.icon_file_svg;
    case SortMenuRow::Icon::Link: return app.icon_link_svg;
    case SortMenuRow::Icon::ArrowDownward: return app.arrow_downward_svg;
    case SortMenuRow::Icon::Folder: return app.icon_folder_svg;
    case SortMenuRow::Icon::EyeOff: return app.icon_eyeoff_svg;
    case SortMenuRow::Icon::List: return app.icon_list_svg;
    case SortMenuRow::Icon::Text: return app.icon_aa_svg;
    case SortMenuRow::Icon::Minus: return app.icon_minus_svg;
    default: return nullptr;
  }
}

static void draw_sort_menu_icon(cairo_t* cr, cairo_surface_t* svg, double x, double y,
                                double size, double r, double g, double b) {
  if (!svg) return;
  double sw = static_cast<double>(cairo_image_surface_get_width(svg));
  double sh = static_cast<double>(cairo_image_surface_get_height(svg));
  double sc = size / std::max(sw, sh);
  cairo_save(cr);
  cairo_set_source_rgba(cr, r, g, b, 1.0);
  cairo_rectangle(cr, x, y, size, size);
  cairo_clip(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, sc, sc);
  cairo_mask_surface(cr, svg, 0, 0);
  cairo_restore(cr);
}

void draw_sort_menu(AppState& app, cairo_t* cr) {
  // Per-pane position helpers
  auto& dm_sort_menu_x = app.active_pane ? app.r_sort_menu_x : app.sort_menu_x;
  auto& dm_sort_menu_y = app.active_pane ? app.r_sort_menu_y : app.sort_menu_y;
  auto& dm_sort_menu_w = app.active_pane ? app.r_sort_menu_w : app.sort_menu_w;
  auto& dm_sort_menu_h = app.active_pane ? app.r_sort_menu_h : app.sort_menu_h;
  auto& dm_sort_menu_hover = app.active_pane ? app.r_sort_menu_hover : app.sort_menu_hover;
  auto& dm_sort_scroll = app.active_pane ? app.r_sort_menu_scroll : app.sort_menu_scroll;
  auto& dm_sort_btn_x = app.active_pane ? app.r_sort_btn_x : app.sort_btn_x;
  auto& dm_sort_btn_w = app.active_pane ? app.r_sort_btn_w : app.sort_btn_w;

  static constexpr int kItemH = kSortMenuItemH;
  static constexpr int kPad = kSortMenuPad;
  int n = sort_menu_row_count();
  int menu_w = 210;

  // Anchored to the sort button's top-right corner (like Nautilus's popover)
  // so it can use the full height below the top bar ("open from top right").
  int menu_top = app.top_bar_height;
  if (app.split_view && app.active_pane) menu_top += app.top_bar_height + app.tab_bar_height;
  int menu_x = dm_sort_btn_x + dm_sort_btn_w - menu_w;
  if (menu_x < 8) menu_x = 8;

  // Cap the height to the room below the top bar; scroll when it overflows so
  // every row stays reachable no matter how small the window is.
  int avail_h = app.height - menu_top - 8;
  int visible = std::clamp((avail_h - kPad * 2) / kItemH, 1, n);
  int max_scroll = std::max(0, n - visible);
  dm_sort_scroll = std::clamp(dm_sort_scroll, 0, max_scroll);
  int menu_h = visible * kItemH + kPad * 2;

  dm_sort_menu_x = menu_x;
  dm_sort_menu_y = menu_top;
  dm_sort_menu_w = menu_w;
  dm_sort_menu_h = menu_h;

  // Shadow
  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, dm_sort_menu_x + s * 2, dm_sort_menu_y + s * 2, menu_w, menu_h, 6);
    cairo_fill(cr);
  }

  // Background
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, dm_sort_menu_x, dm_sort_menu_y, menu_w, menu_h, 6);
  cairo_fill(cr);

  // Outline
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, dm_sort_menu_x + 0.5, dm_sort_menu_y + 0.5, menu_w - 1, menu_h - 1, 5.5);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);

  bool sort_desc = app.cur_tab().sort_descending;
  for (int i0 = 0; i0 < visible; ++i0) {
    int i = dm_sort_scroll + i0;
    int row_y = dm_sort_menu_y + kPad + i0 * kItemH;
    const SortMenuRow& row = sort_menu_row(i);
    bool hovered = (i == dm_sort_menu_hover);

    if (row.kind == SortMenuRow::Kind::Separator) {
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
      cairo_set_line_width(cr, 1);
      cairo_move_to(cr, dm_sort_menu_x + 10, row_y + kItemH / 2 + 0.5);
      cairo_line_to(cr, dm_sort_menu_x + menu_w - 10, row_y + kItemH / 2 + 0.5);
      cairo_stroke(cr);
      continue;
    }

    bool active = false;
    switch (row.kind) {
      case SortMenuRow::Kind::Field:
        active = static_cast<int>(app.cur_tab().sort_field) == row.field;
        break;
      case SortMenuRow::Kind::ToggleDescending:
        active = app.cur_tab().sort_descending;
        break;
      case SortMenuRow::Kind::ToggleFoldersFirst:
        active = app.folders_before_files;
        break;
      case SortMenuRow::Kind::ToggleHiddenLast:
        active = app.sort_hidden_last;
        break;
      case SortMenuRow::Kind::ToggleNatural:
        active = app.sort_natural;
        break;
      case SortMenuRow::Kind::ToggleCaseSensitive:
        active = app.sort_case_sensitive;
        break;
      case SortMenuRow::Kind::GroupField:
        active = app.cur_tab().group_field == row.field;
        break;
      default:
        break;
    }

    if (row.kind == SortMenuRow::Kind::GroupCaption) {
      // Section header with a subtle overline
      cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.18);
      cairo_set_line_width(cr, 1);
      cairo_move_to(cr, dm_sort_menu_x + 10, row_y + 3.0);
      cairo_line_to(cr, dm_sort_menu_x + menu_w - 10, row_y + 3.0);
      cairo_stroke(cr);
      cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                             app.text_secondary_b, 0.85);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 10.5);
      cairo_move_to(cr, dm_sort_menu_x + 12, row_y + kItemH / 2 + 3.5);
      cairo_show_text(cr, row.label);
      cairo_set_font_size(cr, 13);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                              CAIRO_FONT_WEIGHT_NORMAL);
      continue;
    }

    if (hovered) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, dm_sort_menu_x + 4, row_y + 2, menu_w - 8, kItemH - 4, 4);
      cairo_fill(cr);
    }

    // Leading icon for the row kind
    if (row.icon != SortMenuRow::Icon::None) {
      cairo_surface_t* isv = sort_menu_icon_surface(app, row.icon);
      draw_sort_menu_icon(cr, isv, dm_sort_menu_x + 13, row_y + (kItemH - 14) / 2.0, 14,
                          app.text_secondary_r, app.text_secondary_g, app.text_secondary_b);
    }

    // Right-hand state indicator: direction arrow for the active sort field,
    // checkmark for an enabled toggle / active group option.
    bool is_field = row.kind == SortMenuRow::Kind::Field;
    if (is_field && active) {
      draw_sort_menu_icon(cr, sort_desc ? app.arrow_up_svg : app.arrow_down_svg,
                          dm_sort_menu_x + menu_w - 29, row_y + (kItemH - 13) / 2.0, 13,
                          app.accent_r, app.accent_g, app.accent_b);
    } else if (active) {
      draw_sort_menu_icon(cr, app.checkmark_svg,
                          dm_sort_menu_x + menu_w - 29, row_y + (kItemH - 13) / 2.0, 13,
                          app.accent_r, app.accent_g, app.accent_b);
    }

    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, dm_sort_menu_x + 38, row_y + kItemH / 2 + 4);
    cairo_show_text(cr, row.label);
  }

  // Slim scrollbar when the menu overflows the available height
  if (max_scroll > 0) {
    double sbx = dm_sort_menu_x + menu_w - 4.0;
    double sy = dm_sort_menu_y + kPad;
    double shh = menu_h - kPad * 2;
    double thumb_h = std::max(16.0, shh * (double)visible / (double)n);
    double frac = (double)dm_sort_scroll / (double)max_scroll;
    double ty = sy + (shh - thumb_h) * frac;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.5);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, sbx, ty + 1);
    cairo_line_to(cr, sbx, ty + thumb_h - 1);
    cairo_stroke(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
  }
}

// ── column chooser popup ─────────────────────────────────────────

void draw_columns_menu(AppState& app, cairo_t* cr) {
  auto& cm_x = app.active_pane ? app.r_columns_menu_x : app.columns_menu_x;
  auto& cm_y = app.active_pane ? app.r_columns_menu_y : app.columns_menu_y;
  auto& cm_w = app.active_pane ? app.r_columns_menu_w : app.columns_menu_w;
  auto& cm_h = app.active_pane ? app.r_columns_menu_h : app.columns_menu_h;
  auto& cm_hover = app.active_pane ? app.r_columns_menu_hover : app.columns_menu_hover;

  struct ColRow { const char* label; bool* val; };
  ColRow rows[] = {
    {"Owner", &app.col_owner},
    {"Group", &app.col_group},
    {"Permissions", &app.col_perms},
    {"Extension", &app.col_ext},
    {"Link Target", &app.col_target},
  };
  constexpr int kRows = 5;
  int menu_w = 170;
  int menu_h = kRows * kSortMenuItemH + kSortMenuPad * 2;
  cm_w = menu_w;
  cm_h = menu_h;

  for (int s = 3; s >= 0; --s) {
    double a = 0.08 * (1.0 - s / 4.0);
    cairo_set_source_rgba(cr, 0, 0, 0, a);
    draw_rounded_rect(cr, cm_x + s * 2, cm_y + s * 2, menu_w, menu_h, 6);
    cairo_fill(cr);
  }
  double tr, tg, tb;
  wallpaper_tint_surface(app, kPopupWallpaperTint, tr, tg, tb);
  cairo_set_source_rgba(cr, tr, tg, tb, 1.0);
  draw_rounded_rect(cr, cm_x, cm_y, menu_w, menu_h, 6);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.25);
  cairo_set_line_width(cr, 1);
  draw_rounded_rect(cr, cm_x + 0.5, cm_y + 0.5, menu_w - 1, menu_h - 1, 5.5);
  cairo_stroke(cr);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                          CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);

  for (int i = 0; i < kRows; ++i) {
    int row_y = cm_y + kSortMenuPad + i * kSortMenuItemH;
    if (i == cm_hover) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
      draw_rounded_rect(cr, cm_x + 4, row_y, menu_w - 8, kSortMenuItemH, 4);
      cairo_fill(cr);
    }
    if (*rows[i].val) {
      cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 1.0);
      cairo_move_to(cr, cm_x + 14, row_y + kSortMenuItemH / 2 + 4);
      cairo_show_text(cr, "✓ ");
    }
    cairo_set_source_rgba(cr, app.text_r, app.text_g, app.text_b, 1.0);
    cairo_move_to(cr, cm_x + 14 + (*rows[i].val ? 14 : 0),
                  row_y + kSortMenuItemH / 2 + 4);
    cairo_show_text(cr, rows[i].label);
  }
}

} // namespace eh::file_browser

