#include "platform/common/asset/asset_loader.hpp"
#include "platform/common/glyph/material_glyph.hpp"

#include <cairo/cairo.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "stb/stb_image.h"
#include "nanosvg/src/nanosvg.h"
#include "nanosvg/src/nanosvgrast.h"

namespace eh::shell::asset {

static bool file_exists(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "r");
  if (f) { std::fclose(f); return true; }
  return false;
}

static void add_root(std::vector<std::string>& roots, std::string path) {
  if (path.empty()) return;
  if (std::find(roots.begin(), roots.end(), path) == roots.end())
    roots.push_back(std::move(path));
}

// Candidate asset roots, most specific first. Besides the system install dirs
// this covers running straight out of a build tree:
//   build-release/horizon-files -> ../assets
//   build-debug/horizon-files   -> ../assets
// and running from the repo root (assets/). Without these, a dev build shows
// the text placeholders ("F", "S", ...) because every UI SVG fails to load.
static const std::vector<std::string>& asset_roots() {
  static const std::vector<std::string> roots = [] {
    std::vector<std::string> r;

    if (const char* env = std::getenv("EH_ASSETS_DIR"); env && *env)
      add_root(r, env);

    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
      const std::string exe_dir = exe.parent_path().string();
      add_root(r, exe_dir + "/assets");
      add_root(r, exe_dir + "/../assets");
      // Installed layout: <prefix>/bin/horizon-files + <prefix>/share/...
      add_root(r, exe_dir + "/../share/horizon-files/assets");
      // Legacy install location, shared with the Event Horizon desktop files.
      add_root(r, exe_dir + "/../share/event-horizon/assets");
    }

    add_root(r, "assets");
    add_root(r, "src/assets");

    add_root(r, "/usr/local/share/horizon-files/assets");
    add_root(r, "/usr/share/horizon-files/assets");
    add_root(r, "/usr/local/share/event-horizon/assets");
    add_root(r, "/usr/share/event-horizon/assets");
    return r;
  }();
  return roots;
}

static std::string find_asset(const char* subdir, const char* filename) {
  for (const auto& dir : asset_roots()) {
    std::string p = dir + "/" + subdir + "/" + filename;
    if (file_exists(p)) return p;
  }
  return {};
}

static std::string find_asset_root(const char* filename) {
  for (const auto& dir : asset_roots()) {
    std::string p = dir + "/" + filename;
    if (file_exists(p)) return p;
  }
  return {};
}

static cairo_surface_t* load_png(const std::string& path) {
  int w, h, n;
  unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (!data) return nullptr;

  // stb_image outputs RGBA but CAIRO_FORMAT_ARGB32 expects BGRA on little-endian
  for (int i = 0; i < w * h; i++) {
    unsigned char* p = data + i * 4;
    unsigned char tmp = p[0];
    p[0] = p[2];
    p[2] = tmp;
  }

  static const cairo_user_data_key_t kPngKey = {};
  cairo_surface_t* surf = cairo_image_surface_create_for_data(
      data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
  cairo_surface_set_user_data(surf, &kPngKey, data,
      [](void* d) { stbi_image_free(d); });
  return surf;
}

static cairo_surface_t* load_svg(const std::string& path, int max_px) {
  NSVGimage* image = nsvgParseFromFile(path.c_str(), "px", 96.0f);
  if (!image) return nullptr;

  float scale = static_cast<float>(max_px) / std::max(image->width, image->height);
  int w = static_cast<int>(image->width * scale);
  int h = static_cast<int>(image->height * scale);
  if (w < 1) w = 1;
  if (h < 1) h = 1;

  auto* rgba = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(w) * h * 4));
  if (!rgba) { nsvgDelete(image); return nullptr; }

  auto* rast = nsvgCreateRasterizer();
  if (!rast) { std::free(rgba); nsvgDelete(image); return nullptr; }

  nsvgRasterize(rast, image, 0, 0, scale, rgba, w, h, w * 4);
  nsvgDeleteRasterizer(rast);
  nsvgDelete(image);

  // nanosvg outputs RGBA but CAIRO_FORMAT_ARGB32 expects BGRA on little-endian
  for (int i = 0; i < w * h; i++) {
    unsigned char* p = rgba + i * 4;
    unsigned char tmp = p[0];
    p[0] = p[2];
    p[2] = tmp;
  }

  static const cairo_user_data_key_t kSvgKey = {};
  cairo_surface_t* surf = cairo_image_surface_create_for_data(
      rgba, CAIRO_FORMAT_ARGB32, w, h, w * 4);
  cairo_surface_set_user_data(surf, &kSvgKey, rgba,
      [](void* d) { std::free(d); });

  return surf;
}

cairo_surface_t* load_brand_logo_surface() {
  return create_material_glyph_surface("settings", 48);
}

cairo_surface_t* load_launchpad_logo_surface(bool light) {
  std::string name = light ? "Light_Launchpad.svg" : "Dark_Launchpad.svg";
  std::string path = find_asset_root(name.c_str());
  if (path.empty()) {
    name = light ? "Light_Launchpad.png" : "Dark_Launchpad.png";
    path = find_asset_root(name.c_str());
  }
  if (path.empty()) return nullptr;

  if (path.ends_with(".svg"))
    return load_svg(path, 128);
  return load_png(path);
}

cairo_surface_t* load_trash_empty_surface() {
  std::string path = find_asset_root("MacOS-Trash-Empty.png");
  if (path.empty()) return nullptr;
  return load_png(path);
}

cairo_surface_t* load_trash_full_surface() {
  std::string path = find_asset_root("MacOS-Trash-Full.png");
  if (path.empty()) return nullptr;
  return load_png(path);
}

cairo_surface_t* load_asset_svg(const char* subdir, const char* filename, int max_px) {
  std::string path = find_asset(subdir, filename);
  if (path.empty()) return nullptr;
  return load_svg(path, max_px);
}

}
