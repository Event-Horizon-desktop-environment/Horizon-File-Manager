// preview_types.cpp — pure filename-extension predicates for previewable
// types. Zero decode dependencies by design: this TU links into BOTH
// horizon-files (view logic: which rows get thumbnails, icon overlays,
// preview gating) and horizon-thumbnailer (dispatch). The actual decoders
// (libjpeg/WebP/stb, librsvg, poppler, libarchive) live only in the helper.
#include <string>

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
         ext == ".tiff" || ext == ".tif" ||
         // Freedesktop avatar files — plain image data despite the name
         ext == ".face" || ext == ".icon";
}

namespace {
bool ends_ci(const std::string& path, const char* suf, size_t n) {
  if (path.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    char a = path[path.size() - n + i];
    char b = suf[i];
    if ((a >= 'A' && a <= 'Z') ? (a - 'A' + 'a') != b
        : (a >= 'a' && a <= 'z') ? a != b
        : a != b)
      return false;
  }
  return true;
}
}  // namespace

bool is_svg_extension(const std::string& path) {
  return ends_ci(path, ".svg", 4);
}

bool is_pdf_extension(const std::string& path) {
  return ends_ci(path, ".pdf", 4);
}

bool is_epub_extension(const std::string& path) {
  return ends_ci(path, ".epub", 5);
}

}  // namespace eh::file_browser
