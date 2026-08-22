#include "platform/common/icon_cache/icon_cache.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg/src/nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/src/nanosvgrast.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace eh::icons {
static void iconcache_rebuild_search_dirs_locked(IconCacheData& d);
static void iconcache_build_indexes_locked(IconCacheData& d);

static bool icon_debug() {
  static bool on = [] {
    const char* e = std::getenv("EH_ICON_DEBUG");
    return e && *e && e[0] != '0';
  }();
  return on;
}

#define ICON_DBG(...) do { if (icon_debug()) fprintf(stderr, "[icon] " __VA_ARGS__); } while (false)

// ── helpers ──────────────────────────────────────────────────────────

static std::vector<std::string> icon_base_dirs() {
  std::vector<std::string> dirs;
  // Test/bench hook: prepend extra search roots (colon-separated).
  const char* extra = std::getenv("EH_ICON_EXTRA_DIR");
  if (extra && *extra) {
    std::string s(extra);
    size_t pos = 0, end;
    while ((end = s.find(':', pos)) != std::string::npos) {
      if (end > pos) dirs.push_back(s.substr(pos, end - pos));
      pos = end + 1;
    }
    if (pos < s.size()) dirs.push_back(s.substr(pos));
  }
  const char* home = std::getenv("HOME");
  if (home) {
    dirs.push_back(std::string(home) + "/.local/share/icons");
    dirs.push_back(std::string(home) + "/.icons");
  }
  const char* xdg = std::getenv("XDG_DATA_DIRS");
  if (xdg) {
    std::string s(xdg);
    size_t pos = 0, end;
    while ((end = s.find(':', pos)) != std::string::npos) {
      dirs.push_back(s.substr(pos, end - pos) + "/icons");
      pos = end + 1;
    }
    dirs.push_back(s.substr(pos) + "/icons");
  }
  dirs.emplace_back("/usr/local/share/icons");
  dirs.emplace_back("/usr/share/icons");
  if (icon_debug()) {
    fprintf(stderr, "[icon] base dirs:");
    for (auto& d : dirs) fprintf(stderr, " %s", d.c_str());
    fprintf(stderr, "\n");
  }
  return dirs;
}

static cairo_surface_t* ensure_min_size(cairo_surface_t* surf, int min_px) {
  if (!surf) return nullptr;
  int w = cairo_image_surface_get_width(surf);
  int h = cairo_image_surface_get_height(surf);
  if (w >= min_px && h >= min_px) return surf;
  double sc = static_cast<double>(min_px) / std::max(w, h);
  int dw = std::max(1, static_cast<int>(std::round(w * sc)));
  int dh = std::max(1, static_cast<int>(std::round(h * sc)));
  cairo_surface_t* dst = cairo_surface_create_similar_image(surf, CAIRO_FORMAT_ARGB32, dw, dh);
  if (!dst || cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(dst);
    return surf;
  }
  cairo_t* cr = cairo_create(dst);
  cairo_scale(cr, sc, sc);
  cairo_set_source_surface(cr, surf, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(dst);
  cairo_surface_destroy(surf);
  return dst;
}

static cairo_surface_t* load_png(const std::string& path, int min_px) {
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
  return ensure_min_size(surf, min_px);
}

static cairo_surface_t* load_svg(const std::string& path, int size) {
  NSVGimage* image = nsvgParseFromFile(path.c_str(), "px", 96.0f);
  if (!image) return nullptr;

  // Some themes ship SVGs that wrap an embedded base64 PNG via
  // <image xlink:href="data:image/png;base64,..."> instead of real
  // vector paths. nanosvg can't render these, so detect and handle
  // them ourselves.
  if (!image->shapes) {
    nsvgDelete(image);
    static const auto b64_d = []() -> std::array<unsigned char, 256> {
      std::array<unsigned char, 256> t = {};
      for (int i = 0; i < 26; ++i) {
        t[static_cast<unsigned char>('A' + i)] = static_cast<unsigned char>(i);
        t[static_cast<unsigned char>('a' + i)] = static_cast<unsigned char>(26 + i);
      }
      for (int i = 0; i < 10; ++i)
        t[static_cast<unsigned char>('0' + i)] = static_cast<unsigned char>(52 + i);
      t[static_cast<unsigned char>('+')] = 62;
      t[static_cast<unsigned char>('/')] = 63;
      return t;
    }();
    auto b64_c = [&](char c) -> unsigned char { return b64_d[static_cast<unsigned char>(c)]; };
    std::string svg_data;
    {
      std::ifstream ifs(path, std::ios::binary);
      if (ifs) {
        ifs.seekg(0, std::ios::end);
        svg_data.resize(static_cast<std::size_t>(ifs.tellg()));
        ifs.seekg(0, std::ios::beg);
        ifs.read(svg_data.data(), static_cast<std::streamsize>(svg_data.size()));
      }
    }
    const char kNeedle[] = "data:image/png;base64,";
    auto pos = svg_data.find(kNeedle);
    if (pos != std::string::npos) {
      pos += sizeof(kNeedle) - 1;
      auto epos = pos;
      while (epos < svg_data.size() && svg_data[epos] != '"' && svg_data[epos] != '\'')
        ++epos;
      std::string b64;
      for (auto i = pos; i < epos; ++i)
        if (svg_data[i] > ' ') b64.push_back(svg_data[i]);
      std::vector<unsigned char> png_bytes;
      png_bytes.reserve(b64.size() / 4 * 3 + 3);
      for (std::size_t i = 0; i + 3 < b64.size(); i += 4) {
        unsigned char d0 = b64_c(b64[i]), d1 = b64_c(b64[i+1]);
        unsigned char d2 = b64_c(b64[i+2]), d3 = b64_c(b64[i+3]);
        png_bytes.push_back(static_cast<unsigned char>((d0 << 2) | (d1 >> 4)));
        if (b64[i+2] != '=')
          png_bytes.push_back(static_cast<unsigned char>((d1 << 4) | (d2 >> 2)));
        if (b64[i+3] != '=')
          png_bytes.push_back(static_cast<unsigned char>((d2 << 6) | d3));
      }
      int pw = 0, ph = 0, pn = 0;
      unsigned char* png_rgba = stbi_load_from_memory(
          png_bytes.data(), static_cast<int>(png_bytes.size()), &pw, &ph, &pn, 4);
      if (png_rgba) {
        float sc = static_cast<float>(size) / std::max(pw, ph);
        int tw = std::max(1, static_cast<int>(pw * sc));
        int th = std::max(1, static_cast<int>(ph * sc));
        auto* scaled = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(tw) * th * 4));
        if (scaled) {
          cairo_surface_t* src = cairo_image_surface_create_for_data(
              png_rgba, CAIRO_FORMAT_ARGB32, pw, ph, pw * 4);
          for (int i = 0; i < pw * ph; ++i) {
            unsigned char* pp = png_rgba + i * 4;
            unsigned char pt = pp[0]; pp[0] = pp[2]; pp[2] = pt;
            unsigned int pa = pp[3];
            if (pa == 0) { pp[0] = pp[1] = pp[2] = 0; }
            else if (pa < 255) {
              pp[0] = static_cast<unsigned char>((static_cast<unsigned int>(pp[0]) * pa) / 255);
              pp[1] = static_cast<unsigned char>((static_cast<unsigned int>(pp[1]) * pa) / 255);
              pp[2] = static_cast<unsigned char>((static_cast<unsigned int>(pp[2]) * pa) / 255);
            }
          }
          cairo_surface_t* dst = cairo_image_surface_create_for_data(
              scaled, CAIRO_FORMAT_ARGB32, tw, th, tw * 4);
          cairo_t* cr = cairo_create(dst);
          cairo_scale(cr, static_cast<double>(tw) / pw, static_cast<double>(th) / ph);
          cairo_set_source_surface(cr, src, 0, 0);
          cairo_paint(cr);
          cairo_destroy(cr);
          cairo_surface_destroy(src);
          static const cairo_user_data_key_t kEmbedKey = {};
          cairo_surface_set_user_data(dst, &kEmbedKey, scaled,
              [](void* d) { std::free(d); });
          stbi_image_free(png_rgba);
          return dst;
        }
        stbi_image_free(png_rgba);
      }
    }
    return nullptr;
  }

  float scale = static_cast<float>(size) / std::max(image->width, image->height);
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
  // with pre-multiplied alpha. nanosvg gives straight alpha; Cairo requires
  // pre-multiplied, so multiply each colour channel by alpha/255.
  for (int i = 0; i < w * h; i++) {
    unsigned char* p = rgba + i * 4;
    unsigned char tmp = p[0];
    p[0] = p[2];
    p[2] = tmp;
    // Pre-multiply alpha — now channels are B, G, R, A
    unsigned int a = p[3];
    if (a == 0) {
      p[0] = p[1] = p[2] = 0;
    } else if (a < 255) {
      p[0] = static_cast<unsigned char>((static_cast<unsigned int>(p[0]) * a) / 255);
      p[1] = static_cast<unsigned char>((static_cast<unsigned int>(p[1]) * a) / 255);
      p[2] = static_cast<unsigned char>((static_cast<unsigned int>(p[2]) * a) / 255);
    }
  }

  static const cairo_user_data_key_t kSvgKey = {};
  cairo_surface_t* surf = cairo_image_surface_create_for_data(
      rgba, CAIRO_FORMAT_ARGB32, w, h, w * 4);
  cairo_surface_set_user_data(surf, &kSvgKey, rgba,
      [](void* d) { std::free(d); });
  return surf;
}

// ── free functions ───────────────────────────────────────────────────

std::string detect_system_icon_theme() {
  // 0. Environment variable override
  const char* env_theme = std::getenv("EH_ICON_THEME");
  if (env_theme && *env_theme) {
    ICON_DBG("using EH_ICON_THEME override: '%s'\n", env_theme);
    return env_theme;
  }

  // 1. Try gsettings (GNOME/Wayland compositors)
  FILE* gs = popen("gsettings get org.gnome.desktop.interface icon-theme 2>/dev/null", "r");
  if (gs) {
    char buf[128] = {};
    if (fgets(buf, sizeof(buf), gs)) {
      pclose(gs);
      std::string s(buf);
      auto q1 = s.find('\'');
      auto q2 = s.rfind('\'');
      if (q1 != std::string::npos && q2 != q1) {
        auto theme = s.substr(q1 + 1, q2 - q1 - 1);
        if (!theme.empty()) { ICON_DBG("detected theme via gsettings: '%s'\n", theme.c_str()); return theme; }
      }
    } else {
      pclose(gs);
    }
  }

  // 2. Try GTK settings.ini
  const char* home = std::getenv("HOME");
  if (home) {
    std::string path = std::string(home) + "/.config/gtk-3.0/settings.ini";
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
      if (line.starts_with("gtk-icon-theme-name=")) {
        auto theme = line.substr(21);
        if (!theme.empty()) { ICON_DBG("detected theme via gtk ini: '%s'\n", theme.c_str()); return theme; }
      }
    }
  }

  // 3. Fallback
  ICON_DBG("no theme detected via gsettings or gtk ini, using Adwaita\n");
  return "Adwaita";
}

std::vector<ThemeInfo> list_installed_icon_themes() {
  std::vector<ThemeInfo> themes;
  for (const auto& base : icon_base_dirs()) {
    if (!fs::is_directory(base)) continue;
    for (const auto& td : fs::directory_iterator(base)) {
      if (!td.is_directory()) continue;
      ThemeInfo ti;
      ti.path = td.path().string();
      ti.id = td.path().filename().string();
      ti.name = ti.id;
      themes.push_back(std::move(ti));
    }
  }
  return themes;
}

cairo_surface_t* load_theme_preview_icon(const std::string& themeDir, const std::string& iconName, int targetPx) {
  (void)themeDir; (void)iconName; (void)targetPx;
  return nullptr;
}

std::string theme_example_icon_name(const std::string& themeDir) {
  (void)themeDir;
  return "folder";
}

std::vector<std::string> theme_find_any_icons(const std::string& themeDir, int maxCount) {
  (void)themeDir; (void)maxCount;
  return {};
}

std::vector<std::string> list_qt_color_schemes() { return {}; }
std::string detect_current_qt_color_scheme() { return {}; }
bool apply_qt_color_scheme(const std::string&, bool) { return false; }

GtkThemeColors extract_gtk_theme_colors(const std::string&) { return {}; }
void clear_gtk_theme_colors_cache() {}
GtkThemeColors qt_style_preview_colors(const std::string&) { return {}; }
GtkThemeColors qt_color_scheme_preview_colors(const std::string&) { return {}; }
std::string detect_system_gtk_theme() { return "Adwaita"; }
std::vector<GtkThemeInfo> list_installed_gtk_themes() { return {}; }
bool apply_gtk_theme(const std::string&) { return false; }
std::string detect_system_qt_style() { return "Fusion"; }
std::vector<std::string> known_qt_styles() { return {"Fusion", "Breeze"}; }
bool apply_qt_style(const std::string&, bool) { return false; }
std::vector<PlasmaThemeInfo> list_installed_plasma_themes() { return {}; }
GtkThemeColors extract_plasma_theme_colors(const std::string&) { return {}; }
bool apply_plasma_theme(const std::string&) { return false; }
std::vector<CursorThemeInfo> list_installed_cursor_themes() { return {}; }
cairo_surface_t* load_cursor_shape_surface(const std::string&, const std::string&, int) { return nullptr; }
GtkThemeDesign extract_gtk_theme_design(const std::string&) { return {}; }

// ── fallback chains ──────────────────────────────────────────────────
//
// Fallback chains for mime icon names themes commonly omit. Specific sibling
// names first, then family generics so every known file type degrades to a
// sensible icon instead of falling back to the drawn placeholder.
//
// Runtime names are dashed ("application-x-bzip2"); slashed forms are kept
// for absolute robustness across callers.

// Negative-cache keys drop the size bucket: a name that resolves nowhere
// resolves nowhere at every pixel size.
static std::string negative_key_of(const std::string& key) {
  if (key.starts_with("tray:") || key.starts_with("app:") ||
      key.starts_with("exec:")) {
    auto at = key.rfind('@');
    if (at != std::string::npos) return key.substr(0, at);
  }
  return key;
}

static std::vector<std::string> icon_name_candidates(const std::string& icon_name) {
  std::vector<std::string> out{icon_name};
  auto add = [&](const std::string& s) {
    if (s != icon_name && std::find(out.begin(), out.end(), s) == out.end())
      out.push_back(s);
  };
  auto starts = [&](const char* p) { return icon_name.rfind(p, 0) == 0; };
  auto contains = [&](std::initializer_list<const char*> keys) {
    for (auto k : keys)
      if (icon_name.find(k) != std::string::npos) return true;
    return false;
  };

  // Specific sibling aliases
  if (icon_name == "text-x-c++hdr" || icon_name == "text-x-c++-header") {
    add("text-x-chdr"); add("text-x-c++src"); add("text-x-c++");
  } else if (icon_name == "text-x-chdr" || icon_name == "text-x-c-header") {
    add("text-x-csrc"); add("text-x-c");
  } else if (icon_name == "text-x-objchdr") {
    add("text-x-chdr");
  } else if (icon_name == "text-x-objective-c") {
    add("text-x-csrc");
  } else if (icon_name == "text-x-objective-c++") {
    add("text-x-c++");
  } else if (icon_name == "application-x-shellscript" || icon_name == "application-x-sh") {
    add("text-x-script");
  }

  // Family generics: media / fonts resolve within their own family
  if (starts("image/") || starts("image-")) {
    add("image-x-generic");
    return out;
  }
  if (starts("audio/") || starts("audio-")) {
    add("audio-x-generic");
    return out;
  }
  if (starts("video/") || starts("video-")) {
    add("video-x-generic");
    return out;
  }
  if (starts("font/") || starts("font-") || starts("application/x-font") ||
      starts("application/font")) {
    add("font-x-generic");
    return out;
  }

  // application/*: pick a generic by what the name describes
  if (starts("application/") || starts("application-")) {
    if (contains({"zip", "tar", "gzip", "bzip", "compress", "archive", "7z",
                  "rar", "xz", "cab", "cpio", "iso", "cd-image", "disk-image",
                  "package", "deb", "rpm", "flatpak", "snap", "appimage",
                  "zoo", "stuffit", "binhex"})) {
      add("application-zip");
      add("package-x-generic");
    } else if (contains({"officedocument", "opendocument", "msword",
                         "ms-powerpoint", "ms-excel", "ms-publisher",
                         "presentation", "spreadsheet", "wordprocessing",
                         "iwork", "indesign"})) {
      add("x-office-document");
    } else {
      add("text-x-generic");
    }
    return out;
  }

  // text/*: source/script/page degradation for niche languages
  if (starts("text-") || starts("text/")) {
    add("text-x-source");
    add("text-x-script");
    add("text-x-generic");
  }
  return out;
}

static std::vector<std::string> read_inherited_themes(const fs::path& theme_dir) {
  std::vector<std::string> result;
  auto index = theme_dir / "index.theme";
  if (!fs::exists(index)) return result;
  std::ifstream f(index);
  if (!f.is_open()) return result;
  std::string line;
  bool in_icon_theme = false;
  while (std::getline(f, line)) {
    if (line[0] == '[') {
      in_icon_theme = (line == "[Icon Theme]");
      continue;
    }
    if (!in_icon_theme) continue;
    if (line.starts_with("Inherits=")) {
      auto val = line.substr(9);
      size_t start = 0, end;
      while ((end = val.find(',', start)) != std::string::npos) {
        result.push_back(val.substr(start, end - start));
        start = end + 1;
      }
      result.push_back(val.substr(start));
      break;
    }
  }
  return result;
}

// ── IconCache ────────────────────────────────────────────────────────

static constexpr std::size_t kMaxCacheBytes = 64 * 1024 * 1024; // 64 MB

cairo_surface_t* IconCache::load_settings_logo_surface() { return nullptr; }

IconCache::IconCache()
  : d_(std::make_shared<IconCacheData>()) {
  d_->themeOverride = detect_system_icon_theme();
}

IconCache::~IconCache() {
  stop_worker();
}

// Render-size buckets: bounded set of raster sizes so a handful of cache
// entries covers every display size while cutting SVG raster cost ~4-16x
// versus always rendering at 256px.
static int bucket_for(int px) {
  if (px <= 24) return 32;
  if (px <= 48) return 64;
  if (px <= 96) return 128;
  return 256;
}

void IconCache::set_icon_theme(std::string themeId) {
  std::lock_guard<std::mutex> lk(d_->mtx);
  if (d_->themeOverride == themeId) return;
  d_->themeOverride = std::move(themeId);
  d_->resolvedThemeId.clear();
  d_->searchDirsBuilt = false;
  d_->indexesBuilt = false;
  d_->dirIndexes.clear();
  // Drop stale lookups + queued jobs (theme changed under them).
  for (auto& [key, entry] : d_->cache) {
    if (entry.surface) cairo_surface_destroy(entry.surface);
  }
  d_->cache.clear();
  d_->lru.clear();
  d_->totalBytes = 0;
  d_->missLogged.clear();
  d_->execBasenameMiss.clear();
  d_->negativeKeys.clear();
  d_->queue.clear();
  d_->queuedKeys.clear();
  d_->generation++;
}

void IconCache::prewarm_search_dirs() {
  std::lock_guard<std::mutex> lk(d_->mtx);
  iconcache_rebuild_search_dirs_locked(*d_);
}

bool IconCache::refresh_auto_theme_if_needed() { return false; }

void IconCache::clear() {
  std::lock_guard<std::mutex> lk(d_->mtx);
  for (auto& [key, entry] : d_->cache) {
    if (entry.surface) cairo_surface_destroy(entry.surface);
  }
  d_->cache.clear();
  d_->lru.clear();
  d_->totalBytes = 0;
  d_->missLogged.clear();
  d_->execBasenameMiss.clear();
  d_->negativeKeys.clear();
  d_->generation++;
}

// ── theme directory index ────────────────────────────────────────────
//
// One readdir pass per icon category directory builds an in-memory
// name -> path map. After that every icon lookup is pure hash probing —
// zero filesystem syscalls on the draw path.

static const char* const kIndexSubdirs[] = {
    // mime icons first (the hot path for file managers)
    "mimes/scalable",
    "scalable/mimetypes",
    "mimetypes/scalable",
    "mimes/symbolic",
    "mimetypes/symbolic",
    "mimes/16", "mimes/22", "mimes/24", "mimes/32", "mimes/48",
    "mimes/64", "mimes/96", "mimes/128", "mimes/256",
    "mimetypes/16x16", "mimetypes/22x22", "mimetypes/24x24",
    "mimetypes/32x32", "mimetypes/48x48", "mimetypes/64x64",
    "mimetypes/96x96", "mimetypes/128x128", "mimetypes/256x256",
    // then app/places/device/action icons
    "apps/scalable", "places/scalable", "devices/scalable",
    "actions/scalable", "status/scalable", "emblems/scalable",
    "categories/scalable",
    "apps/symbolic", "places/symbolic", "devices/symbolic",
    "actions/symbolic", "status/symbolic",
    "16x16/apps", "16x16/places", "16x16/devices", "16x16/categories",
    "16x16/status", "16x16/emblems", "16x16/actions", "16x16/mimetypes",
    "22x22/apps", "22x22/places", "22x22/devices", "22x22/actions",
    "32x32/apps", "32x32/places", "32x32/devices", "32x32/actions",
    "48x48/apps", "48x48/places", "48x48/devices", "48x48/actions",
    "64x64/apps", "64x64/places", "64x64/devices",
    "128x128/apps", "128x128/places", "128x128/devices",
    "256x256/apps", "256x256/places", "256x256/devices",
};

static bool stem_is_symbolic(const std::string& stem) {
  return stem.size() > 9 && stem.compare(stem.size() - 9, 9, "-symbolic") == 0;
}

static void scan_index_subdir(const fs::path& dir,
                              std::unordered_map<std::string, std::string>& idx) {
  DIR* dp = opendir(dir.c_str());
  if (!dp) return;
  while (struct dirent* de = readdir(dp)) {
    std::string_view fn(de->d_name);
    if (fn.empty() || fn[0] == '.') continue;
    auto dot = fn.rfind('.');
    if (dot == std::string_view::npos) continue;
    std::string_view ext = fn.substr(dot + 1);
    if (ext != "svg" && ext != "png" && ext != "xpm") continue;
    std::string stem(fn.substr(0, dot));
    auto it = idx.find(stem);
    if (it == idx.end()) {
      idx.emplace(std::move(stem), (dir / de->d_name).string());
    } else if (stem_is_symbolic(it->first) && !stem_is_symbolic(stem)) {
      // prefer the full-color icon over the symbolic one
      it->second = (dir / de->d_name).string();
    }
  }
  closedir(dp);
}

// Locked-domain helpers: callers must hold IconCacheData::mtx.
static void iconcache_rebuild_search_dirs_locked(IconCacheData& d) {
  if (d.searchDirsBuilt) return;
  d.searchDirs.clear();

  std::string theme = d.themeOverride;
  if (theme.empty()) theme = "Adwaita";
  ICON_DBG("theme override: '%s'\n", d.themeOverride.c_str());
  ICON_DBG("final theme: '%s'\n", theme.c_str());

  // Collect theme search path with inheritance
  std::vector<std::string> themes_to_check;
  themes_to_check.push_back(theme);

  // Read inheritance from index.theme
  for (const auto& base : icon_base_dirs()) {
    auto td = fs::path(base) / theme;
    if (fs::is_directory(td)) {
      auto inherited = read_inherited_themes(td);
      for (auto& t : inherited) {
        if (!t.empty() && std::find(themes_to_check.begin(), themes_to_check.end(), t) == themes_to_check.end())
          themes_to_check.push_back(t);
      }
      break;
    }
  }

  // Always add hicolor as final fallback
  themes_to_check.push_back("hicolor");

  // Build search dir list
  std::unordered_set<std::string> seen;
  for (const auto& t : themes_to_check) {
    for (const auto& base : icon_base_dirs()) {
      auto td = fs::path(base) / t;
      if (fs::is_directory(td) && seen.insert(td.string()).second) {
        ICON_DBG("  search dir: %s\n", td.c_str());
        d.searchDirs.push_back(td.string());
      }
    }
  }

  d.searchDirsBuilt = true;
}

static void iconcache_build_indexes_locked(IconCacheData& d) {
  if (d.indexesBuilt) return;
  iconcache_rebuild_search_dirs_locked(d);
  d.dirIndexes.assign(d.searchDirs.size(), {});
  for (std::size_t i = 0; i < d.searchDirs.size(); ++i) {
    auto& idx = d.dirIndexes[i];
    for (const char* sub : kIndexSubdirs) {
      scan_index_subdir(fs::path(d.searchDirs[i]) / sub, idx);
    }
  }
  d.indexesBuilt = true;
  d.stIndexBuilds.fetch_add(1, std::memory_order_relaxed);
  ICON_DBG("built indexes for %zu theme dirs\n", d.searchDirs.size());
}

void IconCache::build_indexes_if_needed() {
  // caller holds d_->mtx
  iconcache_build_indexes_locked(*d_);
}

// Probe one theme index for a candidate name. Returns the stored path or {}.
static std::string index_lookup(IconCacheData& d, std::size_t dir_idx,
                                const std::string& name) {
  d.stIndexLookups.fetch_add(1, std::memory_order_relaxed);
  auto& idx = d.dirIndexes[dir_idx];
  if (auto it = idx.find(name); it != idx.end()) return it->second;
  if (auto it = idx.find(name + "-symbolic"); it != idx.end()) return it->second;
  return {};
}

void IconCache::rebuild_search_dirs_if_needed() {
  // caller holds d_->mtx
  iconcache_rebuild_search_dirs_locked(*d_);
}

static void touch_lru_locked(IconCacheData& d, const std::string& key) {
  auto it = d.cache.find(key);
  if (it == d.cache.end()) return;
  auto& entry = it->second;
  d.lru.erase(entry.lru_it);
  d.lru.push_front(key);
  entry.lru_it = d.lru.begin();
}

static void evict_excess_locked(IconCacheData& d) {
  while (d.totalBytes > kMaxCacheBytes && !d.lru.empty()) {
    auto key = d.lru.back();
    d.lru.pop_back();
    auto it = d.cache.find(key);
    if (it != d.cache.end()) {
      if (it->second.surface) cairo_surface_destroy(it->second.surface);
      d.totalBytes -= it->second.bytes;
      d.cache.erase(it);
    }
  }
}

// Core resolution. Caller must NOT hold the mutex.
static void resolve_and_insert(IconCacheData& d, const std::string& key,
                               const std::string& icon_name, int load_size) {
  // Absolute path loads bypass the theme index entirely.
  if (!icon_name.empty() && icon_name[0] == '/') {
    std::error_code ec;
    bool regular = fs::is_regular_file(icon_name, ec);
    cairo_surface_t* surf = nullptr;
    if (regular) {
      surf = icon_name.ends_with(".svg") ? load_svg(icon_name, load_size)
                                         : load_png(icon_name, load_size);
    }
    std::lock_guard<std::mutex> lk(d.mtx);
    if (surf) {
      IconEntry e;
      e.surface = surf;
      e.width = cairo_image_surface_get_width(surf);
      e.height = cairo_image_surface_get_height(surf);
      e.bytes = static_cast<std::size_t>(e.width) * e.height * 4;
      d.totalBytes += e.bytes;
      d.lru.push_front(key);
      e.lru_it = d.lru.begin();
      d.cache.emplace(key, std::move(e));
      d.stRasters.fetch_add(1, std::memory_order_relaxed);
      evict_excess_locked(d);
    } else {
      d.negativeKeys.insert(negative_key_of(key));
    }
    return;
  }

  // Resolve via in-memory indexes: candidates x theme dirs, no syscalls.
  // First job on a worker also builds the theme index — deliberately OUTSIDE
  // mtx so paint threads doing cache checks never wait behind a ~5-17 ms
  // filesystem scan (they just draw placeholders until results land).
  {
    bool need = false;
    {
      std::lock_guard<std::mutex> lk(d.mtx);
      need = !d.indexesBuilt && !d.searchDirs.empty();
    }
    if (need) {
      bool expected = false;
      if (d.indexesBeingBuilt.compare_exchange_strong(expected, true)) {
        std::vector<std::unordered_map<std::string, std::string>> local;
        {
          std::lock_guard<std::mutex> lk(d.mtx);
          iconcache_rebuild_search_dirs_locked(d);
          local.assign(d.searchDirs.size(), {});
        }
        for (std::size_t i = 0; i < local.size(); ++i) {
          for (const char* sub : kIndexSubdirs) {
            scan_index_subdir(fs::path(d.searchDirs[i]) / sub, local[i]);
          }
        }
        std::lock_guard<std::mutex> lk(d.mtx);
        d.dirIndexes = std::move(local);
        d.indexesBuilt = true;
        d.indexesBeingBuilt.store(false);
      } else {
        // Another thread is building; let it finish before looking up.
        for (;;) {
          bool built, building;
          {
            std::lock_guard<std::mutex> lk(d.mtx);
            built = d.indexesBuilt;
            building = d.indexesBeingBuilt.load(std::memory_order_acquire);
          }
          if (built || !building) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }
    }
  }
  std::string path;
  {
    std::lock_guard<std::mutex> lk(d.mtx);
    if (!d.indexesBuilt) iconcache_build_indexes_locked(d);
    for (const auto& cand : icon_name_candidates(icon_name)) {
      for (std::size_t i = 0; i < d.searchDirs.size(); ++i) {
        path = index_lookup(d, i, cand);
        if (!path.empty()) break;
      }
      if (!path.empty()) break;
    }
  }

  cairo_surface_t* surf = nullptr;
  if (!path.empty()) {
    surf = path.ends_with(".svg") ? load_svg(path, load_size)
                                  : load_png(path, load_size);
  }

  std::lock_guard<std::mutex> lk(d.mtx);
  if (surf) {
    IconEntry e;
    e.surface = surf;
    e.width = cairo_image_surface_get_width(surf);
    e.height = cairo_image_surface_get_height(surf);
    e.bytes = static_cast<std::size_t>(e.width) * e.height * 4;
    d.totalBytes += e.bytes;
    d.lru.push_front(key);
    e.lru_it = d.lru.begin();
    d.cache.emplace(key, std::move(e));
    d.stRasters.fetch_add(1, std::memory_order_relaxed);
    evict_excess_locked(d);
    ICON_DBG("  icon FOUND: '%s' -> %s (%dx%d)\n", key.c_str(), path.c_str(),
             e.width, e.height);
  } else {
    d.negativeKeys.insert(negative_key_of(key));
    if (d.missLogged.size() < kMaxIconMissLogged) d.missLogged.insert(key);
    ICON_DBG("  icon NOT FOUND: '%s'\n", key.c_str());
  }
}

const IconEntry* IconCache::resolve_and_cache(const std::string& key,
                                              const std::string& icon_name,
                                              bool is_tray) {
  (void)is_tray;

  {
    std::lock_guard<std::mutex> lk(d_->mtx);
    // Must hold mtx: search dirs feed the index lookups below and the worker
    // may rebuild them concurrently.
    if (!d_->searchDirsBuilt) iconcache_rebuild_search_dirs_locked(*d_);
    auto it = d_->cache.find(key);
    if (it != d_->cache.end()) {
      touch_lru_locked(*d_, key);
      d_->stCacheHits.fetch_add(1, std::memory_order_relaxed);
      ICON_DBG("  cache HIT for '%s'\n", key.c_str());
      return &it->second;
    }
    if (d_->negativeKeys.count(negative_key_of(key))) {
      d_->stNegativeHits.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    build_indexes_if_needed();
  }

  resolve_and_insert(*d_, key, icon_name, 256);

  std::lock_guard<std::mutex> lk(d_->mtx);
  auto it = d_->cache.find(key);
  return it != d_->cache.end() ? &it->second : nullptr;
}

// ── async loading ────────────────────────────────────────────────────

void IconCache::enqueue_async(const std::string& key, const std::string& name, int px) {
  std::unique_lock<std::mutex> lk(d_->mtx);
  if (d_->queuedKeys.count(key)) return;
  d_->queuedKeys.insert(key);
  d_->queue.push_back({key, name, px});
  d_->stAsyncEnqueued.fetch_add(1, std::memory_order_relaxed);
  bool need_start = !d_->workerRunning.load();
  lk.unlock();
  if (need_start) ensure_worker_started();
  d_->cv.notify_one();
}

void IconCache::ensure_worker_started() {
  std::lock_guard<std::mutex> lk(d_->mtx);
  if (d_->workerRunning.load()) return;
  d_->quit.store(false);
  // Claim the running flag BEFORE spawning: it is only cleared by the
  // worker on exit, so concurrent enqueuers can never double-spawn and
  // destroy a joinable std::thread (which would call std::terminate).
  d_->workerRunning.store(true);
  auto d = d_;
  try {
    d_->worker = std::make_unique<std::thread>([d]() {
      for (;;) {
      IconCacheData::PendingLoad job;
      {
        std::unique_lock<std::mutex> lk(d->mtx);
        d->cv.wait(lk, [&] { return d->quit.load() || !d->queue.empty(); });
        if (d->quit.load() && d->queue.empty()) break;
        job = std::move(d->queue.front());
        d->queue.pop_front();
      }
      resolve_and_insert(*d, job.key, job.name, job.px);
      {
        std::lock_guard<std::mutex> lk(d->mtx);
        d->queuedKeys.erase(job.key);
      }
      d->cv.notify_all();
    }
    d->workerRunning.store(false);
  });
  } catch (...) {
    d_->workerRunning.store(false);
    throw;
  }
}

void IconCache::stop_worker() {
  if (!d_) return;
  {
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->quit.store(true);
  }
  d_->cv.notify_all();
  std::thread* t = nullptr;
  {
    std::lock_guard<std::mutex> lk(d_->mtx);
    t = d_->worker.get();
  }
  if (t && t->joinable()) {
    // join outside the lock
    std::thread to_join = std::move(*t);
    d_->worker.reset();
    to_join.join();
  }
}

const IconEntry* IconCache::tray_icon(const std::string& icon_name) {
  std::string key = "tray:" + icon_name;
  return resolve_and_cache(key, icon_name, true);
}

const IconEntry* IconCache::tray_icon_sync(const std::string& icon_name, int pixel_size) {
  int px = bucket_for(pixel_size);
  std::string key = "tray:" + icon_name + "@" + std::to_string(px);
  {
    std::lock_guard<std::mutex> lk(d_->mtx);
    auto it = d_->cache.find(key);
    if (it != d_->cache.end()) {
      touch_lru_locked(*d_, key);
      d_->stCacheHits.fetch_add(1, std::memory_order_relaxed);
      return &it->second;
    }
    if (d_->negativeKeys.count(negative_key_of(key))) {
      d_->stNegativeHits.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    build_indexes_if_needed();
  }
  resolve_and_insert(*d_, key, icon_name, px);
  std::lock_guard<std::mutex> lk(d_->mtx);
  auto it = d_->cache.find(key);
  return it != d_->cache.end() ? &it->second : nullptr;
}

const IconEntry* IconCache::tray_icon(const std::string& icon_name, int pixel_size) {
  static const bool async_disabled = [] {
    const char* e = std::getenv("EH_ICON_ASYNC");
    return e && *e && e[0] == '0';
  }();
  if (async_disabled) return tray_icon_sync(icon_name, pixel_size);

  int px = bucket_for(pixel_size);
  std::string key = "tray:" + icon_name + "@" + std::to_string(px);

  {
    std::lock_guard<std::mutex> lk(d_->mtx);
    auto it = d_->cache.find(key);
    if (it != d_->cache.end()) {
      touch_lru_locked(*d_, key);
      d_->stCacheHits.fetch_add(1, std::memory_order_relaxed);
      return &it->second;
    }
    if (d_->negativeKeys.count(negative_key_of(key))) {
      d_->stNegativeHits.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    // NOTE: no index build here on purpose — the worker thread owns it.
    // Building theme indexes on the paint thread cost ~17 ms on first draw.
  }

  enqueue_async(key, icon_name, px);
  return nullptr; // caller draws placeholder; surface appears next frames
}

const IconEntry* IconCache::app_icon(const std::string& app_id) {
  std::string key = "app:" + app_id;
  return resolve_and_cache(key, app_id, false);
}

const IconEntry* IconCache::app_icon_from_exec_basename(const std::string& exec_basename) {
  std::string key = "exec:" + exec_basename;
  return resolve_and_cache(key, exec_basename, false);
}

// ── test/bench hooks ─────────────────────────────────────────────────

IconCacheStats IconCache::stats() const {
  IconCacheStats s;
  s.cacheHits = d_->stCacheHits.load(std::memory_order_relaxed);
  s.negativeHits = d_->stNegativeHits.load(std::memory_order_relaxed);
  s.indexLookups = d_->stIndexLookups.load(std::memory_order_relaxed);
  s.rasters = d_->stRasters.load(std::memory_order_relaxed);
  s.indexBuilds = d_->stIndexBuilds.load(std::memory_order_relaxed);
  s.asyncEnqueued = d_->stAsyncEnqueued.load(std::memory_order_relaxed);
  return s;
}

bool IconCache::is_negative_cached(const std::string& key) const {
  std::lock_guard<std::mutex> lk(d_->mtx);
  return d_->negativeKeys.count(negative_key_of(key)) != 0;
}

std::size_t IconCache::pending_count() const {
  std::lock_guard<std::mutex> lk(d_->mtx);
  return d_->queue.size();
}

void IconCache::wait_for_pending() {
  std::unique_lock<std::mutex> lk(d_->mtx);
  d_->cv.wait(lk, [&] { return d_->queue.empty(); });
  lk.unlock();
  // Wait until the worker finished inserting the last results too.
  for (;;) {
    bool busy;
    {
      std::lock_guard<std::mutex> lk2(d_->mtx);
      busy = !d_->queuedKeys.empty() || !d_->queue.empty();
    }
    if (!busy) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

}
