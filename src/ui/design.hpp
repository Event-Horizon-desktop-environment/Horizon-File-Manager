#pragma once

// design.hpp — Horizon Design System atoms shared by every surface.
// Extracted from the Properties dialog redesign so dialogs, menus, panels,
// chrome and views all speak the same visual language: grouped 12px cards on
// a matugen-aware fill, small-caps section titles, inset hairlines,
// accent-soft hovers, 40x22 switches, 7px rounded bars, UTF-8-safe
// truncation and friendly size/date/type formatting.
//
// All functions are header-inline; geometry stays with the caller so hit
// testing never drifts from what is drawn.

#include "app/file_browser/app.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace hui::design {

// Paint primitive owned by the app layer; pulled in explicitly so this
// header stays warning-clean outside eh::file_browser.
using eh::file_browser::draw_rounded_rect;

// Matugen-aware card fill: surface/bg mix lifted for elevation.
inline void card_fill(cairo_t* cr, const eh::file_browser::AppState& app, double alpha) {
  double pr = (app.surface_r + app.bg_r) * 0.5 + 0.12;
  double pg = (app.surface_g + app.bg_g) * 0.5 + 0.12;
  double pb = (app.surface_b + app.bg_b) * 0.5 + 0.12;
  cairo_set_source_rgba(cr, pr, pg, pb, alpha);
}

// Filled grouped card, no border (borders add noise at small radii).
inline void section_card(cairo_t* cr, const eh::file_browser::AppState& app, double x, double y,
                         double w, double h, double alpha = 0.55) {
  card_fill(cr, app, alpha);
  draw_rounded_rect(cr, x, y, w, h, 12);
  cairo_fill(cr);
}

// Small-caps section title drawn with its baseline at y; caller advances
// layout by kSectionAdvance afterwards.
inline constexpr int kSectionAdvance = 26;
inline void section_title(cairo_t* cr, const eh::file_browser::AppState& app, double x, double y,
                          const char* title) {
  cairo_set_source_rgba(cr, app.text_secondary_r, app.text_secondary_g,
                        app.text_secondary_b, 0.75);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 11);
  std::string up = title;
  for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  cairo_move_to(cr, x, y);
  cairo_show_text(cr, up.c_str());
}

// Inset horizontal hairline for dividers inside cards and menus.
inline void hairline(cairo_t* cr, const eh::file_browser::AppState& app, double x, double y,
                     double w, double inset = 14) {
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.16);
  cairo_set_line_width(cr, 1);
  cairo_move_to(cr, x + inset, y + 0.5);
  cairo_line_to(cr, x + w - inset, y + 0.5);
  cairo_stroke(cr);
}

// UTF-8-safe end truncation, never splits a multi-byte sequence.
inline std::string clip_end(cairo_t* cr, const std::string& s, double max_w) {
  cairo_text_extents_t te;
  cairo_text_extents(cr, s.c_str(), &te);
  if (s.empty() || te.x_advance <= max_w) return s;
  std::vector<size_t> bounds;
  bounds.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    bounds.push_back(i);
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
                               : ((c & 0xF0) == 0xE0) ? 3
                               : ((c & 0xF8) == 0xF0) ? 4 : 1;
    i += len;
  }
  for (size_t k = bounds.size(); k > 0; --k) {
    std::string out = s.substr(0, bounds[k - 1]) + "\u2026";
    cairo_text_extents(cr, out.c_str(), &te);
    if (te.x_advance <= max_w) return out;
  }
  return "\u2026";
}

// Middle ellipsis for paths: keeps the start and the tail visible.
inline std::string clip_middle(cairo_t* cr, const std::string& s, double max_w) {
  cairo_text_extents_t te;
  cairo_text_extents(cr, s.c_str(), &te);
  if (s.empty() || te.x_advance <= max_w) return s;
  std::vector<size_t> bounds;
  bounds.reserve(s.size() + 1);
  for (size_t i = 0; i < s.size();) {
    bounds.push_back(i);
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
                               : ((c & 0xF0) == 0xE0) ? 3
                               : ((c & 0xF8) == 0xF0) ? 4 : 1;
    i += len;
  }
  bounds.push_back(s.size());
  auto build = [&](size_t keep) {
    size_t head = keep / 2 + keep % 2, tail = keep / 2;
    return s.substr(0, bounds[std::min(head, bounds.size() - 1)]) + "\u2026" +
           s.substr(bounds[bounds.size() - 1 - tail]);
  };
  size_t lo = 0, hi = bounds.size();
  while (lo < hi) {
    size_t mid = (lo + hi + 1) / 2;
    cairo_text_extents(cr, build(mid).c_str(), &te);
    if (te.x_advance <= max_w) lo = mid;
    else hi = mid - 1;
  }
  if (lo == 0) return "\u2026";
  return build(lo);
}

// Extension-preserving truncation for filenames: "archive-2024… .zip".
// UTF-8 safe on both stem and extension.
inline std::string clip_keep_ext(cairo_t* cr, const std::string& s, double max_w) {
  cairo_text_extents_t te;
  cairo_text_extents(cr, s.c_str(), &te);
  if (s.empty() || te.x_advance <= max_w) return s;
  std::string stem = s, ext;
  auto dot = s.rfind('.');
  if (dot != std::string::npos && dot > 0 && s.size() - dot <= 8) {
    ext = s.substr(dot);
    stem = s.substr(0, dot);
  }
  // Character boundaries of the stem.
  std::vector<size_t> bounds;
  bounds.reserve(stem.size());
  for (size_t i = 0; i < stem.size();) {
    bounds.push_back(i);
    unsigned char c = static_cast<unsigned char>(stem[i]);
    size_t len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
                               : ((c & 0xF0) == 0xE0) ? 3
                               : ((c & 0xF8) == 0xF0) ? 4 : 1;
    i += len;
  }
  for (size_t k = bounds.size(); k > 0; --k) {
    std::string out = stem.substr(0, bounds[k - 1]) + "\u2026" + ext;
    cairo_text_extents(cr, out.c_str(), &te);
    if (te.x_advance <= max_w) return out;
  }
  return "\u2026" + ext;
}

// Thousands grouping: 42208 -> "42,208".
inline std::string group_int(long long v) {
  std::string s = std::to_string(v);
  for (long long i = static_cast<long long>(s.size()) - 3; i > 0; i -= 3)
    s.insert(static_cast<size_t>(i), ",");
  return s;
}

inline std::string size_human(uint64_t bytes) {
  char sz[64];
  double v = static_cast<double>(bytes);
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int ui = 0;
  while (v >= 1024.0 && ui < 4) { v /= 1024.0; ++ui; }
  if (ui == 0)
    snprintf(sz, sizeof(sz), "%llu B", (unsigned long long)bytes);
  else
    snprintf(sz, sizeof(sz), "%.1f %s", v, units[ui]);
  return sz;
}

inline std::string size_full(uint64_t bytes) {
  std::string human = size_human(bytes);
  if (bytes < 1024) return human;
  return human + " (" + group_int(static_cast<long long>(bytes)) + " bytes)";
}

// "Sep 23, 2026 · 11:28".
inline std::string date_md(int64_t sec) {
  if (sec == 0) return "";
  struct tm* t = localtime(&sec);
  if (!t) return "";
  static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %d, %d · %02d:%02d", mon[t->tm_mon % 12],
           t->tm_mday, t->tm_year + 1900, t->tm_hour, t->tm_min);
  return buf;
}

// "image/svg+xml" -> "SVG image", "application/pdf" -> "PDF document".
inline std::string friendly_type(const std::string& mime, bool is_dir) {
  if (is_dir) return "Folder";
  if (mime.empty()) return "File";
  if (mime == "inode/directory") return "Folder";
  auto slash = mime.find('/');
  std::string top = (slash == std::string::npos) ? mime : mime.substr(0, slash);
  std::string sub = (slash == std::string::npos) ? "" : mime.substr(slash + 1);
  auto plus = sub.find('+');
  if (plus != std::string::npos) sub = sub.substr(0, plus);
  if (sub.rfind("x-", 0) == 0) sub = sub.substr(2);
  if (sub.empty()) return mime;
  if (top == "image" || top == "video" || top == "audio") {
    for (auto& c : sub) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (sub.size() > 4) {
      sub[0] = sub[0]; // keep all-caps acronyms (TIFF, SVG, PNG)
      for (size_t i = 1; i < sub.size(); ++i)
        sub[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(sub[i])));
      if (sub == "Jpeg") sub = "JPEG";
    }
    const char* kind = top == "image" ? "image" : top == "video" ? "video" : "audio";
    return sub + " " + kind;
  }
  if (mime == "application/pdf") return "PDF document";
  if (sub.size() <= 4) {
    for (auto& c : sub) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return sub;
  }
  std::string pretty;
  bool cap = true;
  for (char c : sub) {
    if (c == '-' || c == '.' || c == '_') { pretty += ' '; cap = true; continue; }
    pretty += cap ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                  : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    cap = false;
  }
  return pretty;
}

// 40x22 toggle switch.
inline void draw_switch(cairo_t* cr, const eh::file_browser::AppState& app, double x, double y, bool on) {
  const double sw_w = 40, sw_h = 22;
  if (on) {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.95);
  } else {
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.55);
  }
  draw_rounded_rect(cr, x, y, sw_w, sw_h, sw_h / 2);
  cairo_fill(cr);
  double knob = on ? x + sw_w - sw_h + 2 : x + 2;
  cairo_set_source_rgba(cr, 1, 1, 1, 0.98);
  cairo_arc(cr, knob + (sw_h - 4) / 2.0, y + sw_h / 2.0, (sw_h - 4) / 2.0 - 1, 0, 2 * M_PI);
  cairo_fill(cr);
}

// Rounded capacity bar: outline track + accent fill.
inline void bar(cairo_t* cr, const eh::file_browser::AppState& app, double x, double y, double w,
                double frac, double h = 7) {
  frac = std::clamp(frac, 0.0, 1.0);
  cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.35);
  draw_rounded_rect(cr, x, y, w, h, h / 2);
  cairo_fill(cr);
  if (frac > 0.001) {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.92);
    draw_rounded_rect(cr, x, y, std::max(2.0, w * frac), h, h / 2);
    cairo_fill(cr);
  }
}

// Accent-soft hover pill for menu rows.
inline void row_hover(cairo_t* cr, const eh::file_browser::AppState& app, double x, double y,
                      double w, double h) {
  cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, 0.16);
  draw_rounded_rect(cr, x, y, w, h, 8);
  cairo_fill(cr);
}

// Buttons: primary (accent fill, white bold label) or ghost.
inline void button(cairo_t* cr, const eh::file_browser::AppState& app, double x, double y, double w,
                   double h, const char* label, bool primary, bool hovered) {
  if (primary) {
    cairo_set_source_rgba(cr, app.accent_r, app.accent_g, app.accent_b, hovered ? 1.0 : 0.9);
  } else {
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, hovered ? 0.10 : 0.04);
  }
  draw_rounded_rect(cr, x, y, w, h, 9);
  cairo_fill(cr);
  if (!primary) {
    cairo_set_source_rgba(cr, app.outline_r, app.outline_g, app.outline_b, 0.3);
    cairo_set_line_width(cr, 1);
    draw_rounded_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 8.5);
    cairo_stroke(cr);
  }
  cairo_set_source_rgba(cr, primary ? 1 : app.text_r, primary ? 1 : app.text_g,
                        primary ? 1 : app.text_b, primary ? 0.95 : 0.9);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         primary ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13);
  cairo_text_extents_t te;
  cairo_text_extents(cr, label, &te);
  cairo_move_to(cr, x + (w - te.x_advance) / 2, y + h / 2 + te.height * 0.35);
  cairo_show_text(cr, label);
}

} // namespace hui::design
