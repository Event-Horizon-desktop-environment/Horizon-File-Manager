#include "platform/common/palette/horizon_colors.hpp"

#include "cpp/cam/hct.h"
#include "cpp/dynamiccolor/dynamic_scheme.h"
#include "cpp/dynamiccolor/material_dynamic_colors.h"
#include "cpp/palettes/tones.h"
#include "cpp/quantize/celebi.h"
#include "cpp/scheme/scheme_content.h"
#include "cpp/scheme/scheme_expressive.h"
#include "cpp/scheme/scheme_fidelity.h"
#include "cpp/scheme/scheme_fruit_salad.h"
#include "cpp/scheme/scheme_monochrome.h"
#include "cpp/scheme/scheme_neutral.h"
#include "cpp/scheme/scheme_rainbow.h"
#include "cpp/scheme/scheme_tonal_spot.h"
#include "cpp/scheme/scheme_vibrant.h"
#include "cpp/score/score.h"
#include "cpp/utils/utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <unistd.h>

// stb_image for non-JPEG image decoding — implementation is in wallpaper_thumbnail.cpp.
#include "stb/stb_image.h"
#include "stb/stb_image_resize2.h"

#ifdef EH_HAVE_LIBJPEG
#include <csetjmp>
#include <jpeglib.h>
#endif

#ifdef EH_HAVE_LCMS2
#include <lcms2.h>
#endif

namespace eh::color {
namespace {

// ── Upstream namespace alias ────────────────────────────────────────────────
namespace mcu = material_color_utilities;

// ── JPEG decode with ICC profile support ────────────────────────────────────
#ifdef EH_HAVE_LIBJPEG

struct EkJpegErrorMgr {
  jpeg_error_mgr pub;
  std::jmp_buf jmp;
};

[[noreturn]] void ek_jpeg_error_exit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<EkJpegErrorMgr*>(cinfo->err);
  std::longjmp(err->jmp, 1);
}

[[nodiscard]] std::vector<uint32_t> decode_jpeg_to_argb(const std::string& path, int& out_w, int& out_h) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return {};

  jpeg_decompress_struct cinfo{};
  EkJpegErrorMgr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = ek_jpeg_error_exit;

  if (setjmp(jerr.jmp)) {
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    return {};
  }

  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);

  // Save APP2 markers before header parsing — ICC profiles live in APP2.
  jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);
  jpeg_read_header(&cinfo, TRUE);

  // ── Collect ICC profile from APP2 markers ────────────────────────────────
  // ICC_PROFILE\0 <seq:u8> <total:u8> <data...>   — 14-byte header, then raw profile data.
  std::map<uint8_t, std::vector<uint8_t>> icc_chunks;
  for (jpeg_saved_marker_ptr m = cinfo.marker_list; m; m = m->next) {
    if (m->marker != JPEG_APP0 + 2 || m->data_length <= 14) continue;
    const unsigned char* d = m->data;
    if (std::memcmp(d, "ICC_PROFILE", 12) != 0) continue;
    const uint8_t seq = d[12];   // 1-based sequence number
    icc_chunks[seq].insert(icc_chunks[seq].end(), d + 14, d + m->data_length);
  }

  // Reassemble ICC profile from ordered chunks.
  std::vector<uint8_t> icc_data;
  for (auto& [seq, chunk] : icc_chunks) {
    icc_data.insert(icc_data.end(), chunk.begin(), chunk.end());
  }

  // ── Decompress to RGB ────────────────────────────────────────────────────
  jpeg_start_decompress(&cinfo);
  out_w = static_cast<int>(cinfo.output_width);
  out_h = static_cast<int>(cinfo.output_height);
  const int row_stride = out_w * static_cast<int>(cinfo.output_components);

  std::vector<uint8_t> rgb(static_cast<size_t>(row_stride) * out_h);
  while (cinfo.output_scanline < cinfo.output_height) {
    uint8_t* row = rgb.data() + static_cast<size_t>(cinfo.output_scanline) * row_stride;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }
  jpeg_destroy_decompress(&cinfo);
  std::fclose(fp);

  // ── Apply ICC profile transform → sRGB ───────────────────────────────────
  // Disabled: matugen (the Rust `image` crate) ignores embedded ICC profiles
  // and scores the raw decoded RGB. Applying an ICC transform here changes the
  // pixel colors and therefore the extracted seed, breaking byte-identical
  // parity. The raw decoder output is intentionally kept to match matugen.
  //
  // Known remaining divergence: PNG decode is lossless and byte-identical to
  // matugen, but JPEG decode is not — matugen uses zune-jpeg (fixed-point IDCT)
  // while this file uses libjpeg-turbo. Their rounding differs by up to ±3 on a
  // small fraction of pixels (~1.7% for IMG_4732.JPG), which can flip the
  // selected seed (e.g. 563925 vs matugen's 704f38). Matching would require a
  // bit-exact zune-jpeg port; not implemented.
#ifdef EH_HAVE_LCMS2
  (void)icc_data;
#endif // EH_HAVE_LCMS2

  // ── Convert RGB → ARGB ───────────────────────────────────────────────────
  const int total = out_w * out_h;
  std::vector<uint32_t> argb(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) {
    const size_t off = static_cast<size_t>(i) * 3;
    argb[i] = argb_from_rgb(rgb[off], rgb[off + 1], rgb[off + 2]);
  }
  return argb;
}

#endif // EH_HAVE_LIBJPEG

// ── Decode images → ARGB vector ──────────────────────────────────────────────
[[nodiscard]] std::vector<uint32_t> decode_image_to_argb(const std::string& path, int& out_w, int& out_h) {
#ifdef EH_HAVE_LIBJPEG
  // Route JPEG files through libjpeg (ICC profile support).
  if (path.size() >= 4) {
    const std::string_view ext(path.data() + path.size() - 4, 4);
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".JPG" || ext == ".JPEG") {
      return decode_jpeg_to_argb(path, out_w, out_h);
    }
  }
#endif

  int channels = 0;
  unsigned char* pixels = stbi_load(path.c_str(), &out_w, &out_h, &channels, 4);
  if (!pixels || out_w <= 0 || out_h <= 0) return {};

  const int total = out_w * out_h;
  std::vector<uint32_t> argb(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) {
    const size_t off = static_cast<size_t>(i) * 4;
    const uint8_t r = pixels[off + 0];
    const uint8_t g = pixels[off + 1];
    const uint8_t b = pixels[off + 2];
    const uint8_t a = pixels[off + 3];
    argb[i] = argb_from_rgb(r, g, b);
    // Preserve alpha in the high byte (stb outputs RGBA, we need ARGB)
    argb[i] = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
              (static_cast<uint32_t>(g) << 8) | b;
  }
  stbi_image_free(pixels);
  return argb;
}

// ── Create DynamicScheme from source color + variant ────────────────────────
//
// NOTE: SchemeVibrant is built inline (instead of using mcu::SchemeVibrant) so
// its neutral-variant palette uses chroma 10. The bundled material-color-utilities
// uses chroma 12 for that palette, but matugen 4.1.0 (material_colors 0.4.2)
// uses 10, which changes every neutral-variant-derived role (surface_variant,
// outline, on_surface_variant, ...). Building it inline keeps the shell's color
// output byte-identical to matugen's scheme-vibrant.
[[nodiscard]] std::unique_ptr<mcu::DynamicScheme> create_scheme(
    mcu::Hct source_hct, SchemeVariant variant, bool is_dark, float contrast_level) {
  switch (variant) {
    case SchemeVariant::TonalSpot:
      return std::make_unique<mcu::SchemeTonalSpot>(source_hct, is_dark, contrast_level);
    case SchemeVariant::Vibrant: {
      const std::vector<double> hues = {0, 41, 61, 101, 131, 181, 251, 301, 360};
      const std::vector<double> secondary_rotations = {18, 15, 10, 12, 15, 18, 15, 12, 12};
      const std::vector<double> tertiary_rotations = {35, 30, 20, 25, 30, 35, 30, 25, 25};
      return std::make_unique<mcu::DynamicScheme>(
          source_hct, mcu::Variant::kVibrant, contrast_level, is_dark,
          mcu::TonalPalette(source_hct.get_hue(), 200.0),
          mcu::TonalPalette(mcu::DynamicScheme::GetRotatedHue(source_hct, hues,
                                                              secondary_rotations),
                            24.0),
          mcu::TonalPalette(mcu::DynamicScheme::GetRotatedHue(source_hct, hues,
                                                              tertiary_rotations),
                            32.0),
          mcu::TonalPalette(source_hct.get_hue(), 10.0),
          mcu::TonalPalette(source_hct.get_hue(), 10.0));
    }
    case SchemeVariant::Expressive:
      return std::make_unique<mcu::SchemeExpressive>(source_hct, is_dark, contrast_level);
    case SchemeVariant::Content:
      return std::make_unique<mcu::SchemeContent>(source_hct, is_dark, contrast_level);
    case SchemeVariant::Fidelity:
      return std::make_unique<mcu::SchemeFidelity>(source_hct, is_dark, contrast_level);
    case SchemeVariant::Monochrome:
      return std::make_unique<mcu::SchemeMonochrome>(source_hct, is_dark, contrast_level);
    case SchemeVariant::Neutral:
      return std::make_unique<mcu::SchemeNeutral>(source_hct, is_dark, contrast_level);
    case SchemeVariant::Rainbow:
      return std::make_unique<mcu::SchemeRainbow>(source_hct, is_dark, contrast_level);
    case SchemeVariant::FruitSalad:
      return std::make_unique<mcu::SchemeFruitSalad>(source_hct, is_dark, contrast_level);
  }
  return std::make_unique<mcu::SchemeContent>(source_hct, is_dark, contrast_level);
}

// ── Convert scheme roles to PaletteResult ───────────────────────────────────
[[nodiscard]] PaletteResult scheme_to_result(const mcu::DynamicScheme& scheme, Argb sourceArgb, bool is_dark) {
  PaletteResult r;
  r.ok = true;
  r.sourceColorArgb = sourceArgb;

  // Map M3 roles to shell chrome fields (matching existing matugen_palette.cpp mapping)
  auto role = [&](uint32_t (mcu::DynamicScheme::*getter)() const) -> Argb {
    return (scheme.*getter)();
  };

  // Dock/Panel fill: SurfaceContainer
  Argb surfaceContainer = role(&mcu::DynamicScheme::GetSurfaceContainer);
  r.dockFillR = mcu::RedFromInt(surfaceContainer) / 255.0f;
  r.dockFillG = mcu::GreenFromInt(surfaceContainer) / 255.0f;
  r.dockFillB = mcu::BlueFromInt(surfaceContainer) / 255.0f;
  r.panelFillR = r.dockFillR;
  r.panelFillG = r.dockFillG;
  r.panelFillB = r.dockFillB;

  // Drawer dim: SurfaceVariant
  Argb surfaceVariant = role(&mcu::DynamicScheme::GetSurfaceVariant);
  r.drawerDimR = mcu::RedFromInt(surfaceVariant) / 255.0f;
  r.drawerDimG = mcu::GreenFromInt(surfaceVariant) / 255.0f;
  r.drawerDimB = mcu::BlueFromInt(surfaceVariant) / 255.0f;

  // Outline
  Argb outline = role(&mcu::DynamicScheme::GetOutline);
  r.outlineR = mcu::RedFromInt(outline) / 255.0f;
  r.outlineG = mcu::GreenFromInt(outline) / 255.0f;
  r.outlineB = mcu::BlueFromInt(outline) / 255.0f;

  // Accent: Primary
  Argb primary = role(&mcu::DynamicScheme::GetPrimary);
  r.accentR = mcu::RedFromInt(primary) / 255.0f;
  r.accentG = mcu::GreenFromInt(primary) / 255.0f;
  r.accentB = mcu::BlueFromInt(primary) / 255.0f;

  // Text: pure white in dark mode, pure black in light mode
  r.textR = is_dark ? 1.0f : 0.0f;
  r.textG = is_dark ? 1.0f : 0.0f;
  r.textB = is_dark ? 1.0f : 0.0f;

  // Notification critical: ErrorContainer / Error
  Argb errorContainer = role(&mcu::DynamicScheme::GetErrorContainer);
  r.notifCriticalBgR = mcu::RedFromInt(errorContainer) / 255.0f;
  r.notifCriticalBgG = mcu::GreenFromInt(errorContainer) / 255.0f;
  r.notifCriticalBgB = mcu::BlueFromInt(errorContainer) / 255.0f;

  Argb error = role(&mcu::DynamicScheme::GetError);
  r.notifCriticalOutlineR = mcu::RedFromInt(error) / 255.0f;
  r.notifCriticalOutlineG = mcu::GreenFromInt(error) / 255.0f;
  r.notifCriticalOutlineB = mcu::BlueFromInt(error) / 255.0f;

  // Full role map (for template engine)
  r.roles[0] = primary;
  r.roles[1] = role(&mcu::DynamicScheme::GetOnPrimary);
  r.roles[2] = role(&mcu::DynamicScheme::GetPrimaryContainer);
  r.roles[3] = role(&mcu::DynamicScheme::GetOnPrimaryContainer);
  r.roles[4] = role(&mcu::DynamicScheme::GetSecondary);
  r.roles[5] = role(&mcu::DynamicScheme::GetOnSecondary);
  r.roles[6] = role(&mcu::DynamicScheme::GetSecondaryContainer);
  r.roles[7] = role(&mcu::DynamicScheme::GetOnSecondaryContainer);
  r.roles[8] = role(&mcu::DynamicScheme::GetTertiary);
  r.roles[9] = role(&mcu::DynamicScheme::GetOnTertiary);
  r.roles[10] = role(&mcu::DynamicScheme::GetTertiaryContainer);
  r.roles[11] = role(&mcu::DynamicScheme::GetOnTertiaryContainer);
  r.roles[12] = role(&mcu::DynamicScheme::GetError);
  r.roles[13] = role(&mcu::DynamicScheme::GetOnError);
  r.roles[14] = role(&mcu::DynamicScheme::GetErrorContainer);
  r.roles[15] = role(&mcu::DynamicScheme::GetOnErrorContainer);
  r.roles[16] = role(&mcu::DynamicScheme::GetSurface);
  r.roles[17] = role(&mcu::DynamicScheme::GetOnSurface);
  r.roles[18] = surfaceVariant;
  r.roles[19] = role(&mcu::DynamicScheme::GetOnSurfaceVariant);
  r.roles[20] = role(&mcu::DynamicScheme::GetSurfaceDim);
  r.roles[21] = role(&mcu::DynamicScheme::GetSurfaceBright);
  r.roles[22] = role(&mcu::DynamicScheme::GetSurfaceContainerLowest);
  r.roles[23] = role(&mcu::DynamicScheme::GetSurfaceContainerLow);
  r.roles[24] = surfaceContainer;
  r.roles[25] = role(&mcu::DynamicScheme::GetSurfaceContainerHigh);
  r.roles[26] = role(&mcu::DynamicScheme::GetSurfaceContainerHighest);
  r.roles[27] = role(&mcu::DynamicScheme::GetInverseSurface);
  r.roles[28] = role(&mcu::DynamicScheme::GetInverseOnSurface);
  r.roles[29] = role(&mcu::DynamicScheme::GetInversePrimary);
  r.roles[30] = outline;
  r.roles[31] = role(&mcu::DynamicScheme::GetOutlineVariant);
  r.roles[32] = role(&mcu::DynamicScheme::GetShadow);
  r.roles[33] = role(&mcu::DynamicScheme::GetScrim);
  r.roles[34] = role(&mcu::DynamicScheme::GetSurfaceTint);

  // Fixed roles (used by some templates, e.g. primary_fixed_dim for buttons)
  r.roles[35] = role(&mcu::DynamicScheme::GetPrimaryFixed);
  r.roles[36] = role(&mcu::DynamicScheme::GetPrimaryFixedDim);
  r.roles[37] = role(&mcu::DynamicScheme::GetOnPrimaryFixed);
  r.roles[38] = role(&mcu::DynamicScheme::GetOnPrimaryFixedVariant);
  r.roles[39] = role(&mcu::DynamicScheme::GetSecondaryFixed);
  r.roles[40] = role(&mcu::DynamicScheme::GetSecondaryFixedDim);
  r.roles[41] = role(&mcu::DynamicScheme::GetOnSecondaryFixed);
  r.roles[42] = role(&mcu::DynamicScheme::GetOnSecondaryFixedVariant);
  r.roles[43] = role(&mcu::DynamicScheme::GetTertiaryFixed);
  r.roles[44] = role(&mcu::DynamicScheme::GetTertiaryFixedDim);
  r.roles[45] = role(&mcu::DynamicScheme::GetOnTertiaryFixed);
  r.roles[46] = role(&mcu::DynamicScheme::GetOnTertiaryFixedVariant);

  return r;
}

} // namespace

// ── Triangle (bilinear) resize, bit-compatible with the `image` crate ────────
// matugen resizes to a fixed 112×112 grid through the Rust `image` crate's
// separable Triangle filter (vertical pass then horizontal pass, f32 weights,
// half-away-from-zero rounding). Reproduce it exactly so the pixel grid fed to
// the quantizer matches matugen. A `volatile` product prevents FMA contraction
// (Rust does not contract `t += v * w` into fused multiply-add).
inline float TriangleAccumulate(float acc, float value, float weight) {
  volatile float product = value * weight;
  return acc + product;
}

void ResizeTriangleToGrid(const std::vector<uint32_t>& src, int sw, int sh,
                          std::vector<uint32_t>& dst, int nw, int nh) {
  // ── Vertical pass: (sw × sh) u8 → (sw × nh) f32 ──────────────────────────
  std::vector<float> tmp(static_cast<size_t>(sw) * nh * 4);
  const float ratio_v = static_cast<float>(sh) / static_cast<float>(nh);
  const float sratio_v = ratio_v < 1.0f ? 1.0f : ratio_v;
  const float src_support_v = sratio_v;

  for (int outy = 0; outy < nh; outy++) {
    float inputy = (static_cast<float>(outy) + 0.5f) * ratio_v;
    long long left = static_cast<long long>(std::floor(inputy - src_support_v));
    left = std::clamp(left, 0LL, static_cast<long long>(sh) - 1);
    long long right = static_cast<long long>(std::ceil(inputy + src_support_v));
    right = std::clamp(right, left + 1, static_cast<long long>(sh));
    inputy -= 0.5f;

    std::vector<float> ws(static_cast<size_t>(right - left));
    float sum = 0.0f;
    for (long long i = left; i < right; i++) {
      const float x = (static_cast<float>(i) - inputy) / sratio_v;
      const float w = std::fabs(x) < 1.0f ? 1.0f - std::fabs(x) : 0.0f;
      ws[static_cast<size_t>(i - left)] = w;
      sum += w;
    }
    for (float& w : ws) w /= sum;

    for (int x = 0; x < sw; x++) {
      float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;
      for (long long i = left; i < right; i++) {
        const uint32_t px = src[static_cast<size_t>(x) + static_cast<size_t>(i) * sw];
        const float w = ws[static_cast<size_t>(i - left)];
        t0 = TriangleAccumulate(t0, static_cast<float>((px >> 24) & 0xff), w);
        t1 = TriangleAccumulate(t1, static_cast<float>((px >> 16) & 0xff), w);
        t2 = TriangleAccumulate(t2, static_cast<float>((px >> 8) & 0xff), w);
        t3 = TriangleAccumulate(t3, static_cast<float>(px & 0xff), w);
      }
      float* out = &tmp[(static_cast<size_t>(x) + static_cast<size_t>(outy) * sw) * 4];
      out[0] = t0;
      out[1] = t1;
      out[2] = t2;
      out[3] = t3;
    }
  }

  // ── Horizontal pass: (sw × nh) f32 → (nw × nh) u8 ────────────────────────
  dst.resize(static_cast<size_t>(nw) * nh);
  const float ratio_h = static_cast<float>(sw) / static_cast<float>(nw);
  const float sratio_h = ratio_h < 1.0f ? 1.0f : ratio_h;
  const float src_support_h = sratio_h;

  for (int outx = 0; outx < nw; outx++) {
    float inputx = (static_cast<float>(outx) + 0.5f) * ratio_h;
    long long left = static_cast<long long>(std::floor(inputx - src_support_h));
    left = std::clamp(left, 0LL, static_cast<long long>(sw) - 1);
    long long right = static_cast<long long>(std::ceil(inputx + src_support_h));
    right = std::clamp(right, left + 1, static_cast<long long>(sw));
    inputx -= 0.5f;

    std::vector<float> ws(static_cast<size_t>(right - left));
    float sum = 0.0f;
    for (long long i = left; i < right; i++) {
      const float x = (static_cast<float>(i) - inputx) / sratio_h;
      const float w = std::fabs(x) < 1.0f ? 1.0f - std::fabs(x) : 0.0f;
      ws[static_cast<size_t>(i - left)] = w;
      sum += w;
    }
    for (float& w : ws) w /= sum;

    for (int y = 0; y < nh; y++) {
      float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;
      for (long long i = left; i < right; i++) {
        const float* px = &tmp[(static_cast<size_t>(i) + static_cast<size_t>(y) * sw) * 4];
        const float w = ws[static_cast<size_t>(i - left)];
        t0 = TriangleAccumulate(t0, px[0], w);
        t1 = TriangleAccumulate(t1, px[1], w);
        t2 = TriangleAccumulate(t2, px[2], w);
        t3 = TriangleAccumulate(t3, px[3], w);
      }
      const auto Round = [](float v) {
        return static_cast<uint8_t>(std::round(std::clamp(v, 0.0f, 255.0f)));
      };
      const size_t idx = static_cast<size_t>(outx) + static_cast<size_t>(y) * nw;
      dst[idx] = (static_cast<uint32_t>(Round(t0)) << 24) |
                 (static_cast<uint32_t>(Round(t1)) << 16) |
                 (static_cast<uint32_t>(Round(t2)) << 8) |
                 static_cast<uint32_t>(Round(t3));
    }
  }
}

// ── Public API ──────────────────────────────────────────────────────────────

PaletteResult generate_palette_from_image(
    const std::string& image_path, SchemeVariant variant, bool is_dark,
    float contrast_level, int max_colors) {
  int w = 0, h = 0;
  std::vector<uint32_t> pixels = decode_image_to_argb(image_path, w, h);
  if (pixels.empty() || w <= 0 || h <= 0) return {};

  // ── Resize to exactly 112×112 (matches matugen / material-colors) ──────────
  // matugen stretches every image to a fixed 112×112 with the `image` crate's
  // Triangle (bilinear) filter before quantization. Mirror that exactly: always
  // resize to a fixed 112×112 grid, regardless of the source aspect ratio,
  // using a bit-compatible Triangle resampler.
  static constexpr int kTargetDim = 112;
  {
    std::vector<uint32_t> resized;
    ResizeTriangleToGrid(pixels, w, h, resized, kTargetDim, kTargetDim);
    pixels = std::move(resized);
    w = kTargetDim;
    h = kTargetDim;
  }

  // Quantize via Celebi (Wu → WSMeans)
  auto quantized = mcu::QuantizeCelebi(pixels, static_cast<uint16_t>(std::min(max_colors, 256)));
  if (quantized.color_to_count.empty()) return {};

  // Drop near-grayscale colors before scoring (matches matugen, which retains
  // only colors with CAM16 chroma >= 5.0).
  for (auto it = quantized.color_to_count.begin();
       it != quantized.color_to_count.end();) {
    if (mcu::Hct(it->first).get_chroma() < 5.0) {
      it = quantized.color_to_count.erase(it);
    } else {
      ++it;
    }
  }
  if (quantized.color_to_count.empty()) return {};

  // Score to pick seed color
  auto options = mcu::ScoreOptions{.desired = 4};
  auto scored = mcu::RankedSuggestions(quantized.color_to_count, options);
  if (scored.empty()) return {};

  const Argb sourceArgb = scored[0];
  const mcu::Hct sourceHct(sourceArgb);

  // Create scheme and convert to result
  auto scheme = create_scheme(sourceHct, variant, is_dark, contrast_level);
  if (!scheme) return {};

  return scheme_to_result(*scheme, sourceArgb, is_dark);
}

PaletteResult generate_palette_from_color(
    Argb source_color, SchemeVariant variant, bool is_dark, float contrast_level) {
  if (source_color == 0) return {};

  const mcu::Hct sourceHct(source_color);
  auto scheme = create_scheme(sourceHct, variant, is_dark, contrast_level);
  if (!scheme) return {};

  return scheme_to_result(*scheme, source_color, is_dark);
}

// ── Cached palette generation ───────────────────────────────────────────────

namespace {

// FNV-1a over raw bytes; enough for a cache key, no security role.
void fnv1a_mix(std::uint64_t& h, const void* data, size_t len) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < len; ++i) {
    h ^= bytes[i];
    h *= 0x100000001b3ULL;
  }
}

template <typename T>
void fnv1a_mix_pod(std::uint64_t& h, const T& v) {
  static_assert(std::is_trivially_copyable_v<T>);
  fnv1a_mix(h, &v, sizeof(T));
}

std::filesystem::path palette_cache_dir() {
  std::string base;
  if (const char* x = ::getenv("XDG_STATE_HOME"); x && *x) {
    base = x;
  } else if (const char* h = ::getenv("HOME"); h && *h) {
    base = std::string(h) + "/.local/state";
  } else {
    return {};
  }
  return std::filesystem::path(base) / "event-horizon" / "palette-cache";
}

// Cache file layout:
//   seed=0x…        picked source color
//   key=<u64>       hash of all inputs that produced it (validates reuse)
Argb palette_cache_load(const std::filesystem::path& file, std::uint64_t key_hash) {
  std::FILE* f = std::fopen(file.c_str(), "rb");
  if (!f) return 0;
  char line[128] = {};
  unsigned seed_val = 0;
  bool have_seed = false;
  unsigned long long stored_key = 0;
  bool have_key = false;
  while (std::fgets(line, sizeof(line), f)) {
    unsigned v = 0;
    if (!have_seed && std::sscanf(line, "seed=%x", &v) == 1) {
      seed_val = v;
      have_seed = true;
      continue;
    }
    if (!have_key &&
        std::sscanf(line, "key=%llu", &stored_key) == 1) {
      have_key = true;
    }
  }
  std::fclose(f);
  if (!have_seed || !have_key) return 0;
  if (stored_key != key_hash) return 0;
  return static_cast<Argb>(seed_val);
}

void palette_cache_store(const std::filesystem::path& file, std::uint64_t key_hash,
                         Argb seed) {
  const std::filesystem::path tmp = file.string() + ".tmp." + std::to_string(::getpid());
  {
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "seed=0x%08x\nkey=%llu\n",
                 static_cast<unsigned>(seed),
                 static_cast<unsigned long long>(key_hash));
    std::fclose(f);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file, ec);
  if (ec) std::filesystem::remove(tmp, ec);
}

} // namespace

PaletteResult generate_palette_from_image_cached(
    const std::string& image_path, SchemeVariant variant, bool is_dark,
    float contrast_level, int max_colors) {
  namespace fs = std::filesystem;
  std::error_code ec;

  // Key: every input that changes the picked seed or the derived palette.
  std::uint64_t key = 0x9e3779b97f4a7c15ULL;
  const auto mix_string = [&key](const std::string& s) {
    fnv1a_mix(key, s.data(), s.size());
    fnv1a_mix_pod(key, '\0');
  };
  mix_string(image_path);
  if (!image_path.empty()) {
    const fs::path p(image_path);
    if (auto t = fs::last_write_time(p, ec); !ec) {
      fnv1a_mix_pod(key, t.time_since_epoch().count());
    }
    if (auto sz = fs::file_size(p, ec); !ec) {
      fnv1a_mix_pod(key, sz);
    }
  }
  fnv1a_mix_pod(key, static_cast<std::uint8_t>(variant));
  fnv1a_mix_pod(key, static_cast<std::uint8_t>(is_dark ? 1 : 0));
  std::uint32_t contrast_bits = 0;
  static_assert(sizeof(contrast_level) == sizeof(contrast_bits));
  std::memcpy(&contrast_bits, &contrast_level, sizeof(contrast_bits));
  fnv1a_mix_pod(key, contrast_bits);
  fnv1a_mix_pod(key, max_colors);

  // L1: process-wide memory cache. Multiple config reloads within a process
  // (startup does several) collapse into at most one image decode.
  static std::mutex mem_mu;
  static std::unordered_map<std::uint64_t, Argb> mem_cache;
  {
    std::lock_guard<std::mutex> lock(mem_mu);
    const auto it = mem_cache.find(key);
    if (it != mem_cache.end()) {
      return generate_palette_from_color(it->second, variant, is_dark, contrast_level);
    }
  }

  // L2: disk cache shared by all shell processes (dock/taskbar/desktop/
  // settings each reload config and would otherwise decode independently).
  Argb seed = 0;
  const fs::path dir = palette_cache_dir();
  if (!dir.empty()) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.palette",
                  static_cast<unsigned long long>(key));
    seed = palette_cache_load(dir / name, key);
    if (seed != 0) {
      std::lock_guard<std::mutex> lock(mem_mu);
      mem_cache.emplace(key, seed);
      return generate_palette_from_color(seed, variant, is_dark, contrast_level);
    }
  }

  // Miss: full decode + quantize + score.
  const PaletteResult result =
      generate_palette_from_image(image_path, variant, is_dark, contrast_level,
                                  max_colors);
  if (!result.ok || result.sourceColorArgb == 0) return result;

  seed = result.sourceColorArgb;
  {
    std::lock_guard<std::mutex> lock(mem_mu);
    mem_cache.emplace(key, seed);
  }
  if (!dir.empty()) {
    std::filesystem::create_directories(dir, ec);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.palette",
                  static_cast<unsigned long long>(key));
    palette_cache_store(dir / name, key, seed);
  }
  return result;
}

SchemeVariant scheme_variant_from_name(std::string_view name) {
  // Normalize to lowercase
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  // Strip "scheme-" prefix if present
  if (lower.size() > 7 && lower.compare(0, 7, "scheme-") == 0) {
    lower.erase(0, 7);
  }

  if (lower == "tonal-spot" || lower == "tonal_spot") return SchemeVariant::TonalSpot;
  if (lower == "vibrant") return SchemeVariant::Vibrant;
  if (lower == "expressive") return SchemeVariant::Expressive;
  if (lower == "content") return SchemeVariant::Content;
  if (lower == "fidelity" || lower == "faithful") return SchemeVariant::Fidelity;
  if (lower == "monochrome") return SchemeVariant::Monochrome;
  if (lower == "neutral") return SchemeVariant::Neutral;
  if (lower == "rainbow") return SchemeVariant::Rainbow;
  if (lower == "fruit-salad" || lower == "fruitsalad") return SchemeVariant::FruitSalad;

  return SchemeVariant::Content; // default
}

const char* scheme_variant_name(SchemeVariant v) {
  switch (v) {
    case SchemeVariant::TonalSpot: return "scheme-tonal-spot";
    case SchemeVariant::Vibrant: return "scheme-vibrant";
    case SchemeVariant::Expressive: return "scheme-expressive";
    case SchemeVariant::Content: return "scheme-content";
    case SchemeVariant::Fidelity: return "scheme-fidelity";
    case SchemeVariant::Monochrome: return "scheme-monochrome";
    case SchemeVariant::Neutral: return "scheme-neutral";
    case SchemeVariant::Rainbow: return "scheme-rainbow";
    case SchemeVariant::FruitSalad: return "scheme-fruit-salad";
  }
  return "scheme-content";
}

std::string hex_from_argb(Argb c) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%06x", c & 0x00FFFFFF);
  return std::string(buf);
}

std::string rgb_string_from_argb(Argb c) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "rgb(%d, %d, %d)",
                red_from_argb(c), green_from_argb(c), blue_from_argb(c));
  return std::string(buf);
}

std::string rgba_string_from_argb(Argb c, uint8_t alpha) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %d)",
                red_from_argb(c), green_from_argb(c), blue_from_argb(c), alpha);
  return std::string(buf);
}

std::string hsl_string_from_argb(Argb c) {
  const float r = red_from_argb(c) / 255.0f;
  const float g = green_from_argb(c) / 255.0f;
  const float b = blue_from_argb(c) / 255.0f;
  const float max = std::max({r, g, b});
  const float min = std::min({r, g, b});
  const float l = (max + min) / 2.0f;

  float h = 0.0f;
  float s = 0.0f;
  if (max != min) {
    const float d = max - min;
    s = l > 0.5f ? d / (2.0f - max - min) : d / (max + min);
    if (max == r)
      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (max == g)
      h = (b - r) / d + 2.0f;
    else
      h = (r - g) / d + 4.0f;
    h /= 6.0f;
  }

  char buf[48];
  std::snprintf(buf, sizeof(buf), "hsl(%.0f, %.0f%%, %.0f%%)",
                h * 360.0f, s * 100.0f, l * 100.0f);
  return std::string(buf);
}

} // namespace eh::color
