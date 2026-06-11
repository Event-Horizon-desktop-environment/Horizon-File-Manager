#include "ux/file_browser/features/image_preview.hpp"

#include "stb/stb_image.h"

#include <webp/decode.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace eh::file_browser {

bool is_image_extension(const std::string& path) {
  auto dot = path.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext;
  for (size_t i = dot; i < path.size(); ++i) {
    char c = path[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    ext += c;
  }
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
         ext == ".gif" || ext == ".bmp" || ext == ".webp" ||
         ext == ".tiff" || ext == ".tif";
}

static cairo_surface_t* create_cairo_surface_from_rgba(
    const unsigned char* data, int w, int h) {
  cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return nullptr;
  }
  int stride = cairo_image_surface_get_stride(surf);
  auto* dst = cairo_image_surface_get_data(surf);
  for (int y = 0; y < h; ++y) {
    auto* row = reinterpret_cast<uint32_t*>(
        dst + static_cast<size_t>(y) * stride);
    const unsigned char* src = data + static_cast<size_t>(y) * 4 * w;
    for (int x = 0; x < w; ++x) {
      unsigned char r = src[0];
      unsigned char g = src[1];
      unsigned char b = src[2];
      unsigned char a = src[3];
      unsigned char pr = static_cast<unsigned char>(
          (static_cast<unsigned>(r) * a + 127) / 255);
      unsigned char pg = static_cast<unsigned char>(
          (static_cast<unsigned>(g) * a + 127) / 255);
      unsigned char pb = static_cast<unsigned char>(
          (static_cast<unsigned>(b) * a + 127) / 255);
      row[x] = (static_cast<uint32_t>(a) << 24) |
               (static_cast<uint32_t>(pr) << 16) |
               (static_cast<uint32_t>(pg) << 8) |
               static_cast<uint32_t>(pb);
      src += 4;
    }
  }
  cairo_surface_mark_dirty_rectangle(surf, 0, 0, w, h);
  return surf;
}

static bool is_webp_ext(const std::string& path) {
  auto dot = path.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext;
  for (size_t i = dot; i < path.size(); ++i) {
    char c = path[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    ext += c;
  }
  return ext == ".webp";
}

static std::vector<unsigned char> read_whole_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::fseek(f, 0, SEEK_END);
  long len = std::ftell(f);
  if (len <= 0) { std::fclose(f); return {}; }
  std::rewind(f);
  std::vector<unsigned char> buf(static_cast<size_t>(len));
  size_t n = std::fread(buf.data(), 1, static_cast<size_t>(len), f);
  std::fclose(f);
  if (static_cast<long>(n) != len) return {};
  return buf;
}

cairo_surface_t* load_image_thumbnail(const std::string& path, int max_px) {
  int w = 0, h = 0, channels = 0;
  unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);

  // stb_image may not support webp in this build — try libwebp directly
  if (!data && is_webp_ext(path)) {
    auto file_data = read_whole_file(path);
    if (!file_data.empty()) {
      uint8_t* webp = WebPDecodeRGBA(file_data.data(),
                                      file_data.size(), &w, &h);
      if (webp && w > 0 && h > 0) {
        cairo_surface_t* result = nullptr;
        cairo_surface_t* src = create_cairo_surface_from_rgba(webp, w, h);
        if (src) {
          double sc = 1.0;
          if (w > max_px || h > max_px)
            sc = std::min(static_cast<double>(max_px) / w,
                          static_cast<double>(max_px) / h);
          int dw = static_cast<int>(std::round(w * sc));
          int dh = static_cast<int>(std::round(h * sc));
          if (dw < 1) dw = 1;
          if (dh < 1) dh = 1;
          cairo_surface_t* dst =
              cairo_surface_create_similar_image(src, CAIRO_FORMAT_ARGB32, dw, dh);
          if (dst && cairo_surface_status(dst) == CAIRO_STATUS_SUCCESS) {
            cairo_t* cr = cairo_create(dst);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, src, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_destroy(cr);
            cairo_surface_flush(dst);
            result = dst;
          } else {
            cairo_surface_destroy(dst);
          }
          cairo_surface_destroy(src);
        }
        WebPFree(webp);
        return result;
      }
      WebPFree(webp);
    }
    return nullptr;
  }

  if (!data) return nullptr;
  if (w <= 0 || h <= 0) {
    stbi_image_free(data);
    return nullptr;
  }

  // Scale down if larger than max_px
  double scale = 1.0;
  if (w > max_px || h > max_px)
    scale = std::min(static_cast<double>(max_px) / w,
                     static_cast<double>(max_px) / h);

  int dw = static_cast<int>(std::round(w * scale));
  int dh = static_cast<int>(std::round(h * scale));
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;

  cairo_surface_t* src = create_cairo_surface_from_rgba(data, w, h);
  stbi_image_free(data);
  if (!src) return nullptr;

  cairo_surface_t* dst =
      cairo_surface_create_similar_image(src, CAIRO_FORMAT_ARGB32, dw, dh);
  if (!dst || cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(src);
    cairo_surface_destroy(dst);
    return nullptr;
  }

  cairo_t* cr = cairo_create(dst);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(src);
  cairo_surface_flush(dst);
  return dst;
}

} // namespace eh::file_browser
