#include "wallpaper/thumbnail/wallpaper_thumbnail.hpp"

namespace eh::wallpaper {

std::vector<std::string> scan_image_files(const std::string& dir) {
  (void)dir;
  return {};
}

bool filename_ends_with_raster_ext(const std::string& path) {
  (void)path;
  return false;
}

cairo_surface_t* load_thumbnail(const std::string& path, int size) {
  (void)path;
  (void)size;
  return nullptr;
}

ThumbnailDecoded decode_thumbnail_to_rgba(const std::string& path, int max_px) {
  (void)path;
  (void)max_px;
  return {};
}

cairo_surface_t* thumbnail_decoded_to_surface(const ThumbnailDecoded& dec) {
  (void)dec;
  return nullptr;
}

void write_thumbnail_disk_cache_maybe(const ThumbnailDecoded& decoded) {
  (void)decoded;
}

std::string normalize_wallpaper_path(const std::string& raw) {
  return raw;
}

}
