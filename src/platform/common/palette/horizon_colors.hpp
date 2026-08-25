#pragma once

// Horizon Colors — native C++ Material Design 3 color engine for Event Horizon.
// Wraps Google's material-color-utilities (Apache-2.0) under eh::color:: namespace.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace eh::color {

// ── Argb color type (matches upstream material_color_utilities::Argb) ──────
using Argb = uint32_t;

// ── Scheme variant identifiers ──────────────────────────────────────────────
enum class SchemeVariant : uint8_t {
  TonalSpot,
  Vibrant,
  Expressive,
  Content,
  Fidelity,
  Monochrome,
  Neutral,
  Rainbow,
  FruitSalad,
};

// ── Palette result from full pipeline ───────────────────────────────────────
struct PaletteResult {
  bool ok = false;

  // Shell chrome colors (float RGB 0–1, matching ShellAppearance field layout)
  float dockFillR = 0.f, dockFillG = 0.f, dockFillB = 0.f;
  float panelFillR = 0.f, panelFillG = 0.f, panelFillB = 0.f;
  float drawerDimR = 0.f, drawerDimG = 0.f, drawerDimB = 0.f;
  float outlineR = 0.f, outlineG = 0.f, outlineB = 0.f;
  float accentR = 0.f, accentG = 0.f, accentB = 0.f;
  float textR = 0.f, textG = 0.f, textB = 0.f;
  float notifCriticalBgR = 0.f, notifCriticalBgG = 0.f, notifCriticalBgB = 0.f;
  float notifCriticalOutlineR = 0.f, notifCriticalOutlineG = 0.f, notifCriticalOutlineB = 0.f;

  // Source color (the seed color picked by scoring)
  Argb sourceColorArgb = 0;

  // Full 53-role M3 palette for template engine and future use
  std::unordered_map<uint8_t, Argb> roles;
};

// ── Palette generation from an image file ───────────────────────────────────
// Decodes the image, quantizes to N colors, scores to pick a seed, generates
// the full M3 palette via the requested scheme variant.
// Returns PaletteResult with ok=true on success.
[[nodiscard]] PaletteResult generate_palette_from_image(
    const std::string& image_path,
    SchemeVariant variant = SchemeVariant::Content,
    bool is_dark = true,
    float contrast_level = 0.0f,
    int max_colors = 128);

// ── Palette generation from a single seed color ─────────────────────────────
[[nodiscard]] PaletteResult generate_palette_from_color(
    Argb source_color,
    SchemeVariant variant = SchemeVariant::Content,
    bool is_dark = true,
    float contrast_level = 0.0f);

// ── Cached palette generation ───────────────────────────────────────────────
// Same as generate_palette_from_image but memoizes the picked seed color:
//  * process-level memory cache, and
//  * disk cache in $XDG_STATE_HOME/event-horizon/palette-cache/, keyed by
//    (path, mtime_ns, size, variant, dark, contrast, max_colors) so the
//    full-resolution image decode runs at most once per wallpaper state and
//    is shared across every shell component that reloads config.
// On cache miss the seed is computed from the image; on any hit the palette
// is rebuilt deterministically via generate_palette_from_color.
[[nodiscard]] PaletteResult generate_palette_from_image_cached(
    const std::string& image_path,
    SchemeVariant variant = SchemeVariant::Content,
    bool is_dark = true,
    float contrast_level = 0.0f,
    int max_colors = 128);

// ── Scheme variant name ↔ enum conversion ───────────────────────────────────
[[nodiscard]] SchemeVariant scheme_variant_from_name(std::string_view name);
[[nodiscard]] const char* scheme_variant_name(SchemeVariant v);

// ── Color format conversion utilities ───────────────────────────────────────
[[nodiscard]] inline uint8_t red_from_argb(Argb c) { return static_cast<uint8_t>((c >> 16) & 0xFF); }
[[nodiscard]] inline uint8_t green_from_argb(Argb c) { return static_cast<uint8_t>((c >> 8) & 0xFF); }
[[nodiscard]] inline uint8_t blue_from_argb(Argb c) { return static_cast<uint8_t>(c & 0xFF); }
[[nodiscard]] inline uint8_t alpha_from_argb(Argb c) { return static_cast<uint8_t>((c >> 24) & 0xFF); }
[[nodiscard]] inline Argb argb_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

[[nodiscard]] std::string hex_from_argb(Argb c);
[[nodiscard]] std::string rgb_string_from_argb(Argb c);
[[nodiscard]] std::string rgba_string_from_argb(Argb c, uint8_t alpha = 255);
[[nodiscard]] std::string hsl_string_from_argb(Argb c);

} // namespace eh::color
