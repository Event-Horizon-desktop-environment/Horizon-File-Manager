#include "../app.hpp"

#include "app/file_browser/features/desktop_icon_parser.hpp"
#include "app/file_browser/features/dirprops.hpp"
#include "app/file_browser/features/query_match.hpp"
#include "app/file_browser/features/recursive_search_worker.hpp"
#include "app/file_browser/features/tab_history.hpp"
#include "app/file_browser/features/video_worker.hpp"
#include "app/file_browser/features/svg_preview.hpp"
#include "app/file_browser/features/pdf_preview.hpp"
#include "app/file_browser/features/epub_preview.hpp"
#include "app/file_browser/features/image_preview.hpp"
#include "app/file_browser/features/view_zoom.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mntent.h>
#include <sys/statvfs.h>

#include "services/udisks2/drive_filter.hpp"

#include <gio/gio.h>

#include "services/udisks2/udisks2_drive_service.hpp"

#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "platform/common/ns/namespaces.hpp"
#include "wayland/surface/layer_surface.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;

namespace eh::file_browser {

void preview_log(const char* fmt, ...);  // defined in draw.cpp

// ── helpers ──────────────────────────────────────────────────────

static std::string desktop_dir() {
  auto h = home_dir();
  return h + "/Desktop";
}

static std::string xdg_user_dir(const char* env, const char* fallback) {
  if (auto* e = std::getenv(env)) return e;
  return home_dir() + "/" + fallback;
}

static bool is_hidden_file(const std::string& name) {
  return !name.empty() && name[0] == '.';
}

// Names listed in a directory's `.hidden` file are treated as hidden too
// (freedesktop convention, honored by Dolphin/Nemo).
std::unordered_set<std::string> read_hidden_file(const std::string& dir) {
  std::unordered_set<std::string> names;
  std::ifstream f(dir + "/.hidden");
  if (!f.is_open()) return names;
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) names.insert(line);
  }
  return names;
}

// Digit-aware "natural" comparison: embedded number runs compare by value,
// so file2 < file10. Honors case sensitivity preference.
static int natural_compare(const std::string& a, const std::string& b,
                           bool case_sensitive) {
  size_t i = 0, j = 0;
  auto fold = [&](unsigned char c) {
    return case_sensitive ? c : static_cast<unsigned char>(std::tolower(c));
  };
  while (i < a.size() && j < b.size()) {
    unsigned char ca = a[i], cb = b[j];
    if (std::isdigit(ca) && std::isdigit(cb)) {
      // Skip leading zeros but remember there was one
      size_t si = i, sj = j;
      while (si < a.size() && a[si] == '0') ++si;
      while (sj < b.size() && b[sj] == '0') ++sj;
      size_t ei = si, ej = sj;
      while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
      while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
      size_t li = ei - si, lj = ej - sj;
      if (li != lj) return li < lj ? -1 : 1;
      while (si < ei) {
        if (a[si] != b[sj]) return a[si] < b[sj] ? -1 : 1;
        ++si; ++sj;
      }
      i = ei; j = ej;
    } else {
      unsigned char fa = fold(ca), fb = fold(cb);
      if (fa != fb) return fa < fb ? -1 : 1;
      ++i; ++j;
    }
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

// Owner/group name lookup with caching to avoid repeated passwd/group hits.
static const std::string& user_name(uid_t uid) {
  static std::unordered_map<uid_t, std::string> cache;
  auto it = cache.find(uid);
  if (it != cache.end()) return it->second;
  struct passwd* pw = getpwuid(uid);
  return cache.emplace(uid, pw ? pw->pw_name : std::to_string(uid)).first->second;
}

static const std::string& group_name(gid_t gid) {
  static std::unordered_map<gid_t, std::string> cache;
  auto it = cache.find(gid);
  if (it != cache.end()) return it->second;
  struct group* gr = getgrgid(gid);
  return cache.emplace(gid, gr ? gr->gr_name : std::to_string(gid)).first->second;
}

static FileType mime_to_file_type(const std::string& mime) {
  if (mime == "inode/directory") return FileType::Folder;

  if (mime.size() > 6 && mime.substr(0, 6) == "image/")
    return FileType::Image;

  if (mime.size() > 6 && mime.substr(0, 6) == "audio/")
    return FileType::Audio;

  if (mime.size() > 6 && mime.substr(0, 6) == "video/")
    return FileType::Video;

  if (mime.size() > 5 && mime.substr(0, 5) == "font/")
    return FileType::Font;

  if (mime.size() > 5 && mime.substr(0, 5) == "text/") {
    if (mime == "text/plain" || mime == "text/csv" || mime == "text/tab-separated-values")
      return FileType::Text;
    if (mime.find("markdown") != std::string::npos || mime == "text/x-markdown")
      return FileType::Markdown;
    if (mime.find("html") != std::string::npos || mime.find("x-php") != std::string::npos ||
        mime == "text/css" || mime == "text/x-scss" || mime == "text/x-sass" ||
        mime == "text/x-less")
      return FileType::Web;
    if (mime.find("x-c++") != std::string::npos || mime.find("x-c") != std::string::npos ||
        mime.find("x-python") != std::string::npos || mime.find("x-ruby") != std::string::npos ||
        mime.find("x-java") != std::string::npos || mime.find("x-rust") != std::string::npos ||
        mime.find("x-go") != std::string::npos || mime.find("x-rsrc") != std::string::npos ||
        mime.find("x-objective") != std::string::npos || mime.find("x-sql") != std::string::npos ||
        mime.find("javascript") != std::string::npos || mime.find("typescript") != std::string::npos ||
        mime.find("x-sh") != std::string::npos || mime.find("x-shell") != std::string::npos ||
        mime.find("x-php") != std::string::npos || mime.find("x-lisp") != std::string::npos ||
        mime.find("x-lua") != std::string::npos || mime.find("x-perl") != std::string::npos ||
        mime.find("x-haskell") != std::string::npos || mime.find("x-erlang") != std::string::npos ||
        mime.find("x-elixir") != std::string::npos || mime.find("x-ocaml") != std::string::npos ||
        mime.find("x-pascal") != std::string::npos || mime.find("x-fortran") != std::string::npos ||
        mime.find("x-coffeescript") != std::string::npos || mime.find("x-sass") != std::string::npos)
      return FileType::Code;
    return FileType::Text;
  }

  if (mime.size() > 12 && mime.substr(0, 12) == "application/") {
    if (mime == "application/pdf" || mime == "application/msword" ||
        mime.find("officedocument") != std::string::npos ||
        mime.find("vnd.openxmlformats") != std::string::npos ||
        mime.find("vnd.oasis.opendocument") != std::string::npos ||
        mime == "application/rtf" || mime == "application/epub+zip" ||
        mime.find("x-mobipocket") != std::string::npos ||
        mime == "application/x-cbr" || mime == "application/x-cbz" ||
        mime == "application/vnd.amazon.ebook" ||
        mime.find("vnd.apple.") != std::string::npos ||
        mime == "application/vnd.ms-powerpoint" ||
        mime == "application/vnd.ms-excel")
      return FileType::Document;

    if (mime == "application/zip" || mime == "application/x-tar" ||
        mime == "application/gzip" || mime == "application/x-bzip2" ||
        mime == "application/x-xz" || mime == "application/x-7z-compressed" ||
        mime == "application/vnd.rar" || mime == "application/x-rar" ||
        mime == "application/zstd" || mime == "application/x-lz4" ||
        mime == "application/x-lzip" || mime == "application/x-lzma" ||
        mime == "application/x-cpio" || mime == "application/x-iso9660-image" ||
        mime == "application/vnd.ms-cab-compressed" ||
        mime == "application/x-archive")
      return FileType::Archive;

    if (mime == "application/x-executable" || mime == "application/x-elf" ||
        mime == "application/x-sharedlib" || mime == "application/x-pie-executable" ||
        mime == "application/vnd.microsoft.portable-executable" ||
        mime == "application/x-ms-dos-executable" ||
        mime == "application/x-msdownload" ||
        mime == "application/x-msi" ||
        mime == "application/x-apple-diskimage" ||
        mime == "application/vnd.debian.binary-package" ||
        mime.find("x-rpm") != std::string::npos ||
        mime.find("x-flatpak") != std::string::npos ||
        mime.find("x-snap") != std::string::npos)
      return FileType::Executable;

    if (mime == "application/json" || mime == "application/xml" ||
        mime == "application/x-yaml" || mime == "application/toml" ||
        mime == "application/x-csv" || mime == "application/x-nfo")
      return FileType::Text;

    return FileType::File;
  }

  return FileType::File;
}

static FileType detect_file_type(const std::string& name, bool is_dir,
                                  const std::string& mime_type,
                                  const std::string& full_path = {}) {
  if (is_dir) return FileType::Folder;

  // Fast path: extension-based detection
  auto dot = name.rfind('.');
  std::string ext;
  if (dot != std::string::npos && dot != name.size() - 1) {
    ext = name.substr(dot + 1);
    for (auto& c : ext) c = std::tolower(c);

    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
        ext == "bmp" || ext == "webp" || ext == "svg" || ext == "avif" ||
        ext == "tif" || ext == "tiff" || ext == "ico" || ext == "heic" ||
        ext == "heif" || ext == "psd" || ext == "xcf" || ext == "ai" ||
        ext == "eps" || ext == "raw" || ext == "cr2" || ext == "nef" ||
        ext == "arw" || ext == "dng" || ext == "orf" || ext == "raf" ||
        ext == "pbm" || ext == "pgm" || ext == "ppm" || ext == "xbm" ||
        ext == "xpm" || ext == "af" || ext == "afphoto" || ext == "afdesign" || ext == "afpub" ||
        ext == "face" || ext == "icon")
      return FileType::Image;

    if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" ||
        ext == "m4a" || ext == "aac" || ext == "opus" || ext == "wma" ||
        ext == "aiff" || ext == "aif" || ext == "alac" || ext == "ac3" ||
        ext == "dts" || ext == "mid" || ext == "midi" || ext == "ape" ||
        ext == "wv" || ext == "tta" || ext == "ra" || ext == "caf")
      return FileType::Audio;

    if (ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov" ||
        ext == "webm" || ext == "m4v" || ext == "wmv" || ext == "flv" ||
        ext == "f4v" || ext == "3gp" || ext == "3g2" || ext == "ogv" ||
        ext == "mpg" || ext == "mpeg" || ext == "mpe" || ext == "ts" ||
        ext == "mts" || ext == "m2ts" || ext == "vob")
      return FileType::Video;

    if (ext == "html" || ext == "htm" || ext == "xhtml" || ext == "css" ||
        ext == "scss" || ext == "sass" || ext == "less" || ext == "php" ||
        ext == "asp" || ext == "aspx" || ext == "jsp" || ext == "wasm")
      return FileType::Web;

    if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mdwn" ||
        ext == "mkd" || ext == "mkdn")
      return FileType::Markdown;

    if (ext == "c" || ext == "cpp" || ext == "cxx" || ext == "cc" ||
        ext == "h" || ext == "hpp" || ext == "hxx" || ext == "hh" ||
        ext == "py" || ext == "pyw" || ext == "rs" || ext == "go" ||
        ext == "java" || ext == "js" || ext == "ts" || ext == "jsx" ||
        ext == "tsx" || ext == "rb" || ext == "php" || ext == "pl" ||
        ext == "pm" || ext == "lua" || ext == "swift" || ext == "kt" ||
        ext == "kts" || ext == "scala" || ext == "clj" || ext == "cljs" ||
        ext == "elm" || ext == "hs" || ext == "dart" || ext == "r" ||
        ext == "m" || ext == "mm" || ext == "cs" || ext == "fs" ||
        ext == "vb" || ext == "sql" || ext == "svelte" || ext == "vue" ||
        ext == "coffee" || ext == "groovy" || ext == "jl" || ext == "nim" ||
        ext == "cob" || ext == "cbl" || ext == "asm" || ext == "s" ||
        ext == "cr" || ext == "zig" || ext == "ex" || ext == "exs" ||
        ext == "erl" || ext == "hrl" || ext == "ml" || ext == "mli" ||
        ext == "re" || ext == "rei" || ext == "tcl" || ext == "d" ||
        ext == "makefile" || ext == "cmake" || ext == "cmakelists")
      return FileType::Code;

    if (ext == "txt" || ext == "conf" || ext == "cfg" ||
        ext == "ini" || ext == "json" || ext == "xml" || ext == "yaml" ||
        ext == "yml" || ext == "log" || ext == "csv" || ext == "tsv" ||
        ext == "toml" || ext == "nfo" || ext == "info" || ext == "tex" ||
        ext == "sty" || ext == "bst")
      return FileType::Text;

    if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "xls" ||
        ext == "xlsx" || ext == "ppt" || ext == "pptx" || ext == "odt" ||
        ext == "ods" || ext == "odp" || ext == "odg" || ext == "odf" ||
        ext == "rtf" || ext == "djvu" || ext == "epub" || ext == "mobi" ||
        ext == "azw" || ext == "azw3" || ext == "cbr" || ext == "cbz" ||
        ext == "pages" || ext == "numbers" || ext == "keynote" ||
        ext == "pub" || ext == "indd")
      return FileType::Document;

    if (ext == "ttf" || ext == "otf" || ext == "woff" || ext == "woff2" ||
        ext == "eot" || ext == "pfa" || ext == "pfb" || ext == "ttc" ||
        ext == "dfont" || ext == "sfd")
      return FileType::Font;

    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "bz2" ||
        ext == "xz" || ext == "7z" || ext == "rar" || ext == "zst" ||
        ext == "zstd" || ext == "lz" || ext == "lz4" || ext == "lzma" ||
        ext == "lzo" || ext == "ar" || ext == "cpio" || ext == "iso" ||
        ext == "cab" || ext == "dmg" || ext == "tgz" || ext == "tbz2" ||
        ext == "txz" || ext == "zoo" || ext == "hqx" || ext == "sit" ||
        ext == "gz2")
      return FileType::Archive;

    if (ext == "sh" || ext == "bin" || ext == "elf" || ext == "exe" ||
        ext == "msi" || ext == "out" || ext == "app" || ext == "run" ||
        ext == "com" || ext == "bat" || ext == "cmd" || ext == "ps1" ||
        ext == "appimage" || ext == "desktop" || ext == "deb" ||
        ext == "rpm" || ext == "appdir" || ext == "flatpak" || ext == "snap")
      return FileType::Executable;
  }

  // Fast GLib MIME guess from filename (no file I/O)
  {
    gboolean uncertain = FALSE;
    gchar* ct = g_content_type_guess(name.c_str(), nullptr, 0, &uncertain);
    if (ct) {
      std::string mime(ct);
      g_free(ct);
      if (mime != "application/octet-stream") {
        FileType ft = mime_to_file_type(mime);
        if (ft != FileType::File) return ft;
      }
    }
  }

  // Content-sniffing fallback for directory browsing (not search results)
  if (!full_path.empty()) {
    FILE* fp = fopen(full_path.c_str(), "rb");
    if (fp) {
      unsigned char buf[512];
      size_t n = fread(buf, 1, sizeof(buf), fp);
      fclose(fp);

      gboolean uncertain = FALSE;
      gchar* ct = g_content_type_guess(name.c_str(), buf, n, &uncertain);
      if (ct) {
        std::string mime(ct);
        g_free(ct);
        FileType ft = mime_to_file_type(mime);
        if (ft != FileType::File) return ft;
      }
    }
  }

  // Old-style mime prefix fallback
  if (mime_type.size() > 6 && mime_type.substr(0, 6) == "image/")
    return FileType::Image;

  return FileType::File;
}

static std::string mime_by_ext(const std::string& path) {
  auto dot = path.rfind('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot + 1);
  for (auto& c : ext) c = static_cast<char>(std::tolower(c));

  // ── Images ──────────────────────────────────────────────────────
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "bmp") return "image/bmp";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "avif") return "image/avif";
  if (ext == "tif" || ext == "tiff") return "image/tiff";
  if (ext == "ico") return "image/x-icon";
  if (ext == "heic" || ext == "heif") return "image/heic";
  if (ext == "psd") return "image/vnd.adobe.photoshop";
  if (ext == "xcf") return "image/x-xcf";
  if (ext == "ai" || ext == "eps") return "application/postscript";
  if (ext == "raw" || ext == "cr2") return "image/x-canon-cr2";
  if (ext == "nef") return "image/x-nikon-nef";
  if (ext == "arw") return "image/x-sony-arw";
  if (ext == "dng") return "image/x-adobe-dng";
  if (ext == "orf") return "image/x-olympus-orf";
  if (ext == "raf") return "image/x-fuji-raf";
  if (ext == "pbm") return "image/x-portable-bitmap";
  if (ext == "pgm") return "image/x-portable-graymap";
  if (ext == "ppm") return "image/x-portable-pixmap";
  if (ext == "xbm") return "image/x-xbitmap";
  if (ext == "xpm") return "image/x-xpixmap";
  if (ext == "af" || ext == "afphoto") return "image/x-affinity-photo";
  if (ext == "afdesign") return "image/x-affinity-designer";
  if (ext == "afpub") return "image/x-affinity-publisher";

  // ── Video ───────────────────────────────────────────────────────
  if (ext == "mp4") return "video/mp4";
  if (ext == "mkv") return "video/x-matroska";
  if (ext == "webm") return "video/webm";
  if (ext == "avi") return "video/x-msvideo";
  if (ext == "mov") return "video/quicktime";
  if (ext == "m4v") return "video/x-m4v";
  if (ext == "wmv") return "video/x-ms-wmv";
  if (ext == "flv" || ext == "f4v") return "video/x-flv";
  if (ext == "3gp") return "video/3gpp";
  if (ext == "3g2") return "video/3gpp2";
  if (ext == "ogv") return "video/ogg";
  if (ext == "mpg" || ext == "mpeg" || ext == "mpe") return "video/mpeg";
  if (ext == "ts" || ext == "mts" || ext == "m2ts") return "video/mp2t";
  if (ext == "vob") return "video/dvd";

  // ── Audio ───────────────────────────────────────────────────────
  if (ext == "mp3") return "audio/mpeg";
  if (ext == "wav") return "audio/wav";
  if (ext == "flac") return "audio/flac";
  if (ext == "ogg") return "audio/ogg";
  if (ext == "m4a") return "audio/mp4";
  if (ext == "aac") return "audio/aac";
  if (ext == "opus") return "audio/opus";
  if (ext == "wma") return "audio/x-ms-wma";
  if (ext == "aiff" || ext == "aif") return "audio/x-aiff";
  if (ext == "alac") return "audio/alac";
  if (ext == "ac3") return "audio/ac3";
  if (ext == "dts") return "audio/vnd.dts";
  if (ext == "mid" || ext == "midi") return "audio/midi";
  if (ext == "ape") return "audio/x-ape";
  if (ext == "wv") return "audio/x-wavpack";
  if (ext == "tta") return "audio/x-tta";
  if (ext == "ra") return "audio/x-realaudio";
  if (ext == "caf") return "audio/x-caf";

  // ── Web ─────────────────────────────────────────────────────────
  if (ext == "html" || ext == "htm" || ext == "xhtml") return "text/html";
  if (ext == "css") return "text/css";
  if (ext == "scss" || ext == "sass") return "text/x-scss";
  if (ext == "less") return "text/x-less";
  if (ext == "php") return "application/x-php";
  if (ext == "asp" || ext == "aspx") return "application/x-asp";
  if (ext == "jsp") return "application/x-jsp";
  if (ext == "wasm") return "application/wasm";

  // ── Markdown ────────────────────────────────────────────────────
  if (ext == "md" || ext == "markdown" || ext == "mdown" || ext == "mdwn" ||
      ext == "mkd" || ext == "mkdn")
    return "text/markdown";

  // ── Code / Source ───────────────────────────────────────────────
  if (ext == "c") return "text/x-c";
  if (ext == "cpp" || ext == "cxx" || ext == "cc") return "text/x-c++";
  if (ext == "h") return "text/x-c-header";
  if (ext == "hpp" || ext == "hxx" || ext == "hh") return "text/x-c++-header";
  if (ext == "py" || ext == "pyw") return "text/x-python";
  if (ext == "rs") return "text/x-rust";
  if (ext == "go") return "text/x-go";
  if (ext == "java") return "text/x-java";
  if (ext == "js") return "text/javascript";
  if (ext == "ts") return "text/typescript";
  if (ext == "jsx") return "text/javascript";
  if (ext == "tsx") return "text/typescript";
  if (ext == "rb") return "text/x-ruby";
  if (ext == "pl" || ext == "pm") return "text/x-perl";
  if (ext == "lua") return "text/x-lua";
  if (ext == "swift") return "text/x-swift";
  if (ext == "kt" || ext == "kts") return "text/x-kotlin";
  if (ext == "scala") return "text/x-scala";
  if (ext == "clj" || ext == "cljs") return "text/x-clojure";
  if (ext == "elm") return "text/x-elm";
  if (ext == "hs") return "text/x-haskell";
  if (ext == "dart") return "text/x-dart";
  if (ext == "r") return "text/x-r";
  if (ext == "m") return "text/x-objective-c";
  if (ext == "mm") return "text/x-objective-c++";
  if (ext == "cs") return "text/x-csharp";
  if (ext == "fs") return "text/x-fsharp";
  if (ext == "vb") return "text/x-vb";
  if (ext == "sql") return "text/x-sql";
  if (ext == "svelte") return "text/x-svelte";
  if (ext == "vue") return "text/x-vue";
  if (ext == "coffee") return "text/x-coffeescript";
  if (ext == "groovy") return "text/x-groovy";
  if (ext == "jl") return "text/x-julia";
  if (ext == "nim") return "text/x-nim";
  if (ext == "cob" || ext == "cbl") return "text/x-cobol";
  if (ext == "asm" || ext == "s") return "text/x-assembly";
  if (ext == "cr") return "text/x-crystal";
  if (ext == "zig") return "text/x-zig";
  if (ext == "ex" || ext == "exs") return "text/x-elixir";
  if (ext == "erl" || ext == "hrl") return "text/x-erlang";
  if (ext == "ml" || ext == "mli") return "text/x-ocaml";
  if (ext == "re" || ext == "rei") return "text/x-reason";
  if (ext == "tcl") return "text/x-tcl";
  if (ext == "d") return "text/x-d";
  if (ext == "makefile" || ext == "cmake") return "text/x-cmake";

  // ── Text / config ───────────────────────────────────────────────
  if (ext == "txt") return "text/plain";
  if (ext == "conf" || ext == "cfg") return "text/x-config";
  if (ext == "ini") return "text/x-ini";
  if (ext == "log") return "text/x-log";
  if (ext == "csv") return "text/csv";
  if (ext == "tsv") return "text/tab-separated-values";
  if (ext == "yaml" || ext == "yml") return "text/yaml";
  if (ext == "toml") return "text/toml";
  if (ext == "json") return "application/json";
  if (ext == "xml") return "application/xml";
  if (ext == "nfo" || ext == "info") return "text/x-nfo";
  if (ext == "tex" || ext == "sty" || ext == "bst") return "text/x-tex";

  // ── Documents ───────────────────────────────────────────────────
  if (ext == "pdf") return "application/pdf";
  if (ext == "djvu") return "image/vnd.djvu";
  if (ext == "epub") return "application/epub+zip";
  if (ext == "mobi" || ext == "azw" || ext == "azw3") return "application/x-mobipocket-ebook";
  if (ext == "cbr" || ext == "cbz") return "application/x-comic-book";
  if (ext == "doc") return "application/msword";
  if (ext == "docx") return "application/vnd.openxmlformats-officedocument.wordprocessingml";
  if (ext == "xls") return "application/vnd.ms-excel";
  if (ext == "xlsx") return "application/vnd.openxmlformats-officedocument.spreadsheetml";
  if (ext == "ppt") return "application/vnd.ms-powerpoint";
  if (ext == "pptx") return "application/vnd.openxmlformats-officedocument.presentationml";
  if (ext == "odt") return "application/vnd.oasis.opendocument.text";
  if (ext == "ods") return "application/vnd.oasis.opendocument.spreadsheet";
  if (ext == "odp") return "application/vnd.oasis.opendocument.presentation";
  if (ext == "odg") return "application/vnd.oasis.opendocument.graphics";
  if (ext == "odf") return "application/vnd.oasis.opendocument.formula";
  if (ext == "rtf") return "application/rtf";
  if (ext == "pages") return "application/x-iwork-pages-sffpages";
  if (ext == "numbers") return "application/x-iwork-numbers-sffnumbers";
  if (ext == "keynote") return "application/x-iwork-keynote-sffkey";
  if (ext == "pub") return "application/x-mspublisher";
  if (ext == "indd") return "application/x-indesign";

  // ── Fonts ───────────────────────────────────────────────────────
  if (ext == "ttf") return "font/ttf";
  if (ext == "otf") return "font/otf";
  if (ext == "woff") return "font/woff";
  if (ext == "woff2") return "font/woff2";
  if (ext == "eot") return "application/vnd.ms-fontobject";
  if (ext == "pfa" || ext == "pfb") return "font/type1";
  if (ext == "ttc") return "font/collection";
  if (ext == "dfont") return "font/x-dfont";
  if (ext == "sfd") return "font/x-sfd";

  // ── Archives ────────────────────────────────────────────────────
  if (ext == "zip") return "application/zip";
  if (ext == "tar") return "application/x-tar";
  if (ext == "gz" || ext == "tgz") return "application/gzip";
  if (ext == "bz2" || ext == "tbz2") return "application/x-bzip2";
  if (ext == "xz" || ext == "txz") return "application/x-xz";
  if (ext == "7z") return "application/x-7z-compressed";
  if (ext == "rar") return "application/vnd.rar";
  if (ext == "zst" || ext == "zstd") return "application/zstd";
  if (ext == "lz") return "application/x-lzip";
  if (ext == "lz4") return "application/x-lz4";
  if (ext == "lzma") return "application/x-lzma";
  if (ext == "ar") return "application/x-archive";
  if (ext == "cpio") return "application/x-cpio";
  if (ext == "iso") return "application/x-iso9660-image";
  if (ext == "cab") return "application/vnd.ms-cab-compressed";
  if (ext == "dmg") return "application/x-apple-diskimage";
  if (ext == "zoo") return "application/x-zoo";
  if (ext == "hqx") return "application/mac-binhex40";
  if (ext == "sit") return "application/x-stuffit";

  // ── Executables ─────────────────────────────────────────────────
  if (ext == "sh") return "application/x-sh";
  if (ext == "bash") return "application/x-sh";
  if (ext == "bin") return "application/x-binary";
  if (ext == "elf") return "application/x-elf";
  if (ext == "exe") return "application/x-ms-dos-executable";
  if (ext == "msi") return "application/x-msi";
  if (ext == "out") return "application/x-object";
  if (ext == "appimage") return "application/x-appimage";
  if (ext == "desktop") return "application/x-desktop";
  if (ext == "deb") return "application/vnd.debian.binary-package";
  if (ext == "rpm") return "application/x-rpm";
  if (ext == "flatpak") return "application/x-flatpak";
  if (ext == "snap") return "application/x-snap";
  if (ext == "bat" || ext == "cmd") return "application/x-msdos-program";
  if (ext == "ps1") return "application/x-powershell";

  return {};
}

// ── app state constructor ────────────────────────────────────────

AppState::AppState() {
  tabs.emplace_back();
  tabs[0].current_path = home_dir();
}

AppState::~AppState() {
  if (arrow_left_svg) cairo_surface_destroy(arrow_left_svg);
  if (arrow_right_svg) cairo_surface_destroy(arrow_right_svg);
  if (arrow_up_svg) cairo_surface_destroy(arrow_up_svg);
  if (search_svg) cairo_surface_destroy(search_svg);
  if (folder_search_svg) cairo_surface_destroy(folder_search_svg);
  if (mounted_svg) cairo_surface_destroy(mounted_svg);
  if (icon_desktop_svg) cairo_surface_destroy(icon_desktop_svg);
  if (icon_documents_svg) cairo_surface_destroy(icon_documents_svg);
  if (icon_downloads_svg) cairo_surface_destroy(icon_downloads_svg);
  if (icon_music_svg) cairo_surface_destroy(icon_music_svg);
  if (icon_pictures_svg) cairo_surface_destroy(icon_pictures_svg);
  if (icon_videos_svg) cairo_surface_destroy(icon_videos_svg);
  if (icon_publicshare_svg) cairo_surface_destroy(icon_publicshare_svg);
  if (icon_templates_svg) cairo_surface_destroy(icon_templates_svg);
}

// ── home_dir ─────────────────────────────────────────────────────

std::string home_dir() {
  if (auto* h = std::getenv("HOME")) return h;
  if (auto* pw = getpwuid(getuid())) return pw->pw_dir;
  return "/";
}

// ── directory listing ────────────────────────────────────────────

static void reset_scroll_and_selection(AppState& app) {
  app.cur_tab().hover_idx = -1;
  app.cur_tab().selected_idx = -1;
  app.cur_tab().multi_selected.clear();
  app.cur_tab().scroll_px = 0;
  app.cur_tab().scroll_smooth_current = 0.0;
  app.cur_tab().scroll_smooth_target = 0.0;
}

static bool matches_filter(const AppState& app, const FileEntry& entry) {
  int ft = app.active_pane ? app.r_filter_type_idx : app.filter_type_idx;
  int fs = app.active_pane ? app.r_filter_size_idx : app.filter_size_idx;
  int fd = app.active_pane ? app.r_filter_date_idx : app.filter_date_idx;
  // Type filter
  if (ft > 0) {
    FileType target = static_cast<FileType>(ft - 1);
    if (entry.type != target) return false;
  }
  // Size filter
  if (fs > 0) {
    uint64_t s = entry.size;
    switch (fs) {
      case 1: if (s >= 10240) return false; break;
      case 2: if (s < 10240 || s >= 102400) return false; break;
      case 3: if (s < 102400 || s >= 1048576) return false; break;
      case 4: if (s < 1048576 || s >= 10485760) return false; break;
      case 5: if (s < 10485760 || s >= 104857600) return false; break;
      case 6: if (s < 104857600) return false; break;
    }
  }
  // Date filter
  if (fd > 0) {
    int64_t mod = entry.modified_sec;
    if (mod == 0) return false;
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int64_t boundary = 0;
    switch (fd) {
      case 1: { // Today
        struct tm tm_today = tm_now;
        tm_today.tm_hour = 0; tm_today.tm_min = 0; tm_today.tm_sec = 0;
        boundary = mktime(&tm_today);
        break;
      }
      case 2: { // This week (start of Monday)
        int days_since_monday = (tm_now.tm_wday + 6) % 7;
        struct tm tm_week = tm_now;
        tm_week.tm_hour = 0; tm_week.tm_min = 0; tm_week.tm_sec = 0;
        tm_week.tm_mday -= days_since_monday;
        boundary = mktime(&tm_week);
        break;
      }
      case 3: { // This month
        struct tm tm_month = tm_now;
        tm_month.tm_hour = 0; tm_month.tm_min = 0; tm_month.tm_sec = 0;
        tm_month.tm_mday = 1;
        boundary = mktime(&tm_month);
        break;
      }
      case 4: { // This year
        struct tm tm_year = tm_now;
        tm_year.tm_hour = 0; tm_year.tm_min = 0; tm_year.tm_sec = 0;
        tm_year.tm_mday = 1; tm_year.tm_mon = 0;
        boundary = mktime(&tm_year);
        break;
      }
    }
    if (mod < boundary) return false;
  }
  return true;
}

// Thread-safe predicate for the recursive search worker: applies the filter
// dropdown's type/size/date selections to raw stat data. Runs on the worker
// thread — must not touch AppState.
bool search_predicate_passes(int ft, int fs, int fd, const std::string& path,
                             const std::string& name, bool is_dir,
                             uint64_t size, int64_t mtime) {
  if (ft > 0) {
    std::string mime = is_dir ? "inode/directory" : mime_by_ext(path);
    FileType t = detect_file_type(name, is_dir, mime, path);
    if (t != static_cast<FileType>(ft - 1)) return false;
  }
  if (fs > 0) {
    switch (fs) {
      case 1: if (size >= 10240) return false; break;
      case 2: if (size < 10240 || size >= 102400) return false; break;
      case 3: if (size < 102400 || size >= 1048576) return false; break;
      case 4: if (size < 1048576 || size >= 10485760) return false; break;
      case 5: if (size < 10485760 || size >= 104857600) return false; break;
      case 6: if (size < 104857600) return false; break;
    }
  }
  if (fd > 0) {
    if (mtime == 0) return false;
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int64_t boundary = 0;
    switch (fd) {
      case 1: {
        struct tm tm_today = tm_now;
        tm_today.tm_hour = 0; tm_today.tm_min = 0; tm_today.tm_sec = 0;
        boundary = mktime(&tm_today);
        break;
      }
      case 2: {
        int days_since_monday = (tm_now.tm_wday + 6) % 7;
        struct tm tm_week = tm_now;
        tm_week.tm_hour = 0; tm_week.tm_min = 0; tm_week.tm_sec = 0;
        tm_week.tm_mday -= days_since_monday;
        boundary = mktime(&tm_week);
        break;
      }
      case 3: {
        struct tm tm_month = tm_now;
        tm_month.tm_hour = 0; tm_month.tm_min = 0; tm_month.tm_sec = 0;
        tm_month.tm_mday = 1;
        boundary = mktime(&tm_month);
        break;
      }
      case 4: {
        struct tm tm_year = tm_now;
        tm_year.tm_hour = 0; tm_year.tm_min = 0; tm_year.tm_sec = 0;
        tm_year.tm_mday = 1; tm_year.tm_mon = 0;
        boundary = mktime(&tm_year);
        break;
      }
    }
    if (mtime < boundary) return false;
  }
  return true;
}

// Query match honoring the filter-bar mode (plain/glob/regex) and case
// toggle. Empty query matches everything.
static bool query_matches_entry(const AppState& app, const std::string& name) {
  const auto& q = app.active_pane ? app.r_search_query : app.search_query;
  if (q.empty()) return true;
  int mode = app.active_pane ? app.r_search_mode : app.search_mode;
  bool cs = app.active_pane ? app.r_search_case_sensitive : app.search_case_sensitive;
  bool valid = true;
  return name_matches(name, q, mode, cs, &valid);
}

// Restart the active pane's search with current query/mode/case/filter
// settings (used after mode or option changes).
void restart_active_search(AppState& app) {
  const auto& q = app.active_pane ? app.r_search_query : app.search_query;
  bool rec = app.active_pane ? app.r_recursive_search_active
                             : app.recursive_search_active;
  if (q.empty() || !(app.search_active || app.recursive_search_active ||
                     app.r_search_active || app.r_recursive_search_active)) {
    recursive_search_worker().cancel();
    return;
  }
  app.cur_tab().entries.clear();
  app.cur_tab().visible_entries.clear();

  SearchOptions opt;
  opt.mode = app.active_pane ? app.r_search_mode : app.search_mode;
  opt.case_sensitive = app.active_pane ? app.r_search_case_sensitive
                                       : app.search_case_sensitive;
  int ft = app.active_pane ? app.r_filter_type_idx : app.filter_type_idx;
  int fs = app.active_pane ? app.r_filter_size_idx : app.filter_size_idx;
  int fd = app.active_pane ? app.r_filter_date_idx : app.filter_date_idx;
  if (ft > 0 || fs > 0 || fd > 0) {
    opt.predicate = [ft, fs, fd](const std::string& path,
                                 const std::string& name, bool is_dir,
                                 uint64_t size, int64_t mtime) {
      return search_predicate_passes(ft, fs, fd, path, name, is_dir, size,
                                     mtime);
    };
  }
  std::string root = rec ? home_dir() : app.cur_tab().current_path;
  recursive_search_worker().start_search(root, q, opt);
}

void reset_search_filters(AppState& app) {
  (app.active_pane ? app.r_filter_type_idx : app.filter_type_idx) = 0;
  (app.active_pane ? app.r_filter_size_idx : app.filter_size_idx) = 0;
  (app.active_pane ? app.r_filter_date_idx : app.filter_date_idx) = 0;
  (app.active_pane ? app.r_filter_dropdown_section : app.filter_dropdown_section) = 0;
  (app.active_pane ? app.r_filter_dropdown_hover : app.filter_dropdown_hover) = -1;
}

void trigger_search_on_filter_change(AppState& app) {
  if ((app.active_pane ? app.r_filter_type_idx : app.filter_type_idx) == 0 && (app.active_pane ? app.r_filter_size_idx : app.filter_size_idx) == 0 && (app.active_pane ? app.r_filter_date_idx : app.filter_date_idx) == 0) {
    // No filters — just re-apply the existing apply_filter logic
    apply_filter(app);
    return;
  }
  // Rebuild visible_entries from entries with filters
  app.cur_tab().visible_entries.clear();
  for (int i = 0; i < static_cast<int>(app.cur_tab().entries.size()); ++i) {
    if (!app.show_hidden && app.cur_tab().entries[i].is_hidden) continue;
    if (!query_matches_entry(app, app.cur_tab().entries[i].name)) continue;
    if (!matches_filter(app, app.cur_tab().entries[i])) continue;
    app.cur_tab().visible_entries.push_back(i);
  }
  reset_scroll_and_selection(app);
}

void apply_filter(AppState& app) {
  if (app.cur_tab().current_path == "computer://") return;
  app.cur_tab().visible_entries.clear();
  for (int i = 0; i < static_cast<int>(app.cur_tab().entries.size()); ++i) {
    if (!app.show_hidden && app.cur_tab().entries[i].is_hidden) continue;
    if (!query_matches_entry(app, app.cur_tab().entries[i].name)) continue;
    if (!matches_filter(app, app.cur_tab().entries[i])) continue;
    app.cur_tab().visible_entries.push_back(i);
  }
  reset_scroll_and_selection(app);
}

void reload_dir(AppState& app) {
  reset_preview(app);
  hide_tooltip(app);
  app.cur_tab().entries.clear();

  // Virtual computer view — no directory to load
  if (app.cur_tab().current_path == "computer://") {
    app.cur_tab().view_mode = ViewMode::Computer;
    app.computer_needs_refresh = true;
    return;
  }

  if (!fs::is_directory(app.cur_tab().current_path)) {
    app.cur_tab().current_path = home_dir();
  }

  // Reset view mode if coming from computer view
  if (app.cur_tab().view_mode == ViewMode::Computer) {
    app.cur_tab().view_mode = app.last_browser_view_mode;
  }

  // ── Per-folder .directory view properties ──
  app.cur_tab().dynamic_view_done = false;
  if (app.per_folder_props) {
    DirProps dp;
    if (read_dir_props(app.cur_tab().current_path, &dp)) {
      auto& tab = app.cur_tab();
      if (dp.has_mode && dp.mode >= 0 && dp.mode <= 4 &&
          dp.mode != static_cast<int>(ViewMode::Computer)) {
        tab.view_mode = static_cast<ViewMode>(dp.mode);
        app.last_browser_view_mode = tab.view_mode;
      }
      if (dp.has_sort && dp.sort_field >= 0 &&
          dp.sort_field <= static_cast<int>(SortField::LinkTarget)) {
        tab.sort_field = static_cast<SortField>(dp.sort_field);
        tab.sort_descending = dp.sort_descending;
      }
      if (dp.has_flags) {
        app.sort_natural = dp.natural;
        app.sort_case_sensitive = dp.case_sensitive;
        app.sort_hidden_last = dp.hidden_last;
        app.folders_before_files = dp.folders_first;
      }
      if (dp.has_group && dp.group_field >= 0 && dp.group_field <= 4) {
        tab.group_field = dp.group_field;
        tab.group_by_type = (dp.group_field == 1);
      }
      if (dp.has_zoom && dp.zoom_level >= 0 && dp.zoom_level <= 16)
        apply_zoom_pct(app, zoom_pct_for_level(dp.zoom_level));
      if (dp.has_hidden) app.show_hidden = dp.show_hidden;
    }
  }

  DIR* dir = opendir(app.cur_tab().current_path.c_str());
  if (!dir) return;

  const std::unordered_set<std::string> hidden_names =
      read_hidden_file(app.cur_tab().current_path);

  struct dirent* dent;
  while ((dent = readdir(dir)) != nullptr) {
    std::string name = dent->d_name;
    if (name == "." || name == "..") continue;

    std::string full = app.cur_tab().current_path + "/" + name;

    struct stat st{};
    bool stat_ok = stat(full.c_str(), &st) == 0;

    FileEntry e;
    e.name = name;
    e.path = full;
    e.in_hidden_file = hidden_names.count(name) > 0;
    e.is_hidden = is_hidden_file(name) || e.in_hidden_file;
    e.is_symlink = (dent->d_type == DT_LNK);

    if (dent->d_type == DT_DIR) {
      e.is_dir = true;
    } else if (stat_ok) {
      e.is_dir = S_ISDIR(st.st_mode);
    }

    if (stat_ok) {
      e.size = static_cast<uint64_t>(st.st_size);
      e.modified_sec = static_cast<int64_t>(st.st_mtime);
      e.mode = st.st_mode;
      e.readable = (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) != 0 ||
                   geteuid() == 0;
      e.writable = (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0 ||
                   geteuid() == 0;
      e.owner = user_name(st.st_uid);
      e.group = group_name(st.st_gid);
      e.owned_by_root = (st.st_uid == 0);
    }

    // Symlink target (for LinkTarget sorting / emblems)
    if (e.is_symlink) {
      char buf[4096];
      ssize_t n = readlink(full.c_str(), buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = '\0';
        e.link_target = buf;
      }
    }

    // Lowercase extension without the dot ("tar.gz" keeps full suffix chain)
    if (!e.is_dir) {
      auto dot = name.rfind('.');
      if (dot != std::string::npos && dot + 1 < name.size()) {
        e.extension = name.substr(dot + 1);
        for (auto& c : e.extension) c = std::tolower(c);
      }
    }

    if (!e.is_dir) {
      e.mime_type = mime_by_ext(full);
    }

    e.type = detect_file_type(name, e.is_dir, e.mime_type, full);

    // XDG user-directory icons from system theme
    if (e.is_dir) {
      static const std::string xdg_home = home_dir();
      if      (full == xdg_home + "/Desktop")    e.icon_name = "user-desktop";
      else if (full == xdg_home + "/Documents")  e.icon_name = "folder-documents";
      else if (full == xdg_home + "/Downloads")  e.icon_name = "folder-download";
      else if (full == xdg_home + "/Music")      e.icon_name = "folder-music";
      else if (full == xdg_home + "/Pictures")   e.icon_name = "folder-pictures";
      else if (full == xdg_home + "/Videos")     e.icon_name = "folder-videos";
      else if (full == xdg_home + "/Public")     e.icon_name = "folder-publicshare";
      else if (full == xdg_home + "/Templates")  e.icon_name = "folder-templates";
    }

    if (!e.is_dir) {
      auto dot = name.rfind('.');
      if (dot != std::string::npos) {
        std::string ext = name.substr(dot + 1);
        for (auto& c : ext) c = std::tolower(c);
        if (ext == "desktop") {
          auto icon = parse_desktop_icon(full);
          if (!icon.empty()) e.icon_name = std::move(icon);
        }
      }
    }

    // Derive Freedesktop icon name from MIME type (icon themes use `media-subtype` format)
    if (e.icon_name.empty() && !e.mime_type.empty()) {
      std::string icon = e.mime_type;
      for (auto& c : icon) if (c == '/') c = '-';
      e.icon_name = std::move(icon);
    }

    app.cur_tab().entries.push_back(std::move(e));
  }
  closedir(dir);

  // ── Group By: rank + label helpers (field: 0=None 1=Type 2=Name 3=Date 4=Size) ──
  auto group_rank = [&](const FileEntry& e) -> int {
    switch (app.cur_tab().group_field) {
      case 1: return static_cast<int>(e.type);
      case 2: {
        if (e.name.empty()) return 0;
        unsigned char c0 = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(e.name[0])));
        return (c0 >= 'a' && c0 <= 'z') ? c0 : ('z' + 1);
      }
      case 3: {
        std::time_t now = std::time(nullptr);
        double age_s = difftime(now, e.modified_sec);
        if (age_s < 0) age_s = 0;
        if (age_s < 86400) return 0;                     // Today
        if (age_s < 172800) return 1;                    // Yesterday
        if (age_s < 7 * 86400) return 2;                 // This Week
        if (age_s < 30 * 86400) return 3;                // This Month
        if (age_s < 365 * 86400) return 4;               // This Year
        return 5;                                        // Earlier
      }
      case 4:
        if (e.is_dir) return 0;                          // Folders
        if (e.size < 100ull * 1024) return 1;            // Small
        if (e.size < 10ull * 1024 * 1024) return 2;      // Medium
        if (e.size < 1024ull * 1024 * 1024) return 3;    // Large
        return 4;                                        // Huge
      default: return 0;
    }
  };

  std::sort(app.cur_tab().entries.begin(), app.cur_tab().entries.end(),
    [&](const FileEntry& a, const FileEntry& b) {
      if (app.folders_before_files && a.is_dir != b.is_dir) return a.is_dir;

      // Hidden entries optionally sort after everything else
      if (app.sort_hidden_last && a.is_hidden != b.is_hidden) return !a.is_hidden;

      // Group By: primary sort key when enabled
      if (app.cur_tab().group_field > 0) {
        int ga = group_rank(a);
        int gb = group_rank(b);
        if (ga != gb) return ga < gb;
      }

      auto name_cmp = [&](const std::string& x, const std::string& y) -> int {
        if (app.sort_natural)
          return natural_compare(x, y, app.sort_case_sensitive);
        if (app.sort_case_sensitive)
          return x.compare(y);
        // Plain case-folded lexicographic
        size_t n = std::min(x.size(), y.size());
        for (size_t k = 0; k < n; ++k) {
          unsigned char cx = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(x[k])));
          unsigned char cy = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(y[k])));
          if (cx != cy) return cx < cy ? -1 : 1;
        }
        return x.size() < y.size() ? -1 : (x.size() > y.size() ? 1 : 0);
      };

      int cmp = 0;
      switch (app.cur_tab().sort_field) {
        case SortField::Name:
          cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Size:
          if (a.size < b.size) cmp = -1;
          else if (a.size > b.size) cmp = 1;
          break;
        case SortField::Modified:
          if (a.modified_sec < b.modified_sec) cmp = -1;
          else if (a.modified_sec > b.modified_sec) cmp = 1;
          break;
        case SortField::FirstModified:
          if (a.modified_sec < b.modified_sec) cmp = -1;
          else if (a.modified_sec > b.modified_sec) cmp = 1;
          break;
        case SortField::LastModified:
          if (a.modified_sec > b.modified_sec) cmp = -1;
          else if (a.modified_sec < b.modified_sec) cmp = 1;
          break;
        case SortField::Type:
          cmp = static_cast<int>(a.type) - static_cast<int>(b.type);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Owner:
          cmp = a.owner.compare(b.owner);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Group:
          cmp = a.group.compare(b.group);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Permissions:
          cmp = (a.mode & 0777) - (b.mode & 0777);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::Extension:
          cmp = a.extension.compare(b.extension);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
        case SortField::LinkTarget:
          cmp = a.link_target.compare(b.link_target);
          if (cmp == 0) cmp = name_cmp(a.name, b.name);
          break;
      }
      // FirstModified/LastModified have fixed directions, ignore sort_descending
      if (app.cur_tab().sort_field == SortField::FirstModified)
        return cmp < 0;
      if (app.cur_tab().sort_field == SortField::LastModified)
        return cmp > 0;
      return app.cur_tab().sort_descending ? cmp > 0 : cmp < 0;
    });

  app.cur_tab().visible_entries.clear();
  for (int i = 0; i < static_cast<int>(app.cur_tab().entries.size()); ++i) {
    if (!app.show_hidden && app.cur_tab().entries[i].is_hidden) continue;
    if (!query_matches_entry(app, app.cur_tab().entries[i].name)) continue;
    app.cur_tab().visible_entries.push_back(i);
  }

  reset_scroll_and_selection(app);

  // Track directory mtime for auto-refresh
  struct stat dir_st;
  if (stat(app.cur_tab().current_path.c_str(), &dir_st) == 0)
    app.cur_tab().dir_mtime = static_cast<int64_t>(dir_st.st_mtime);

  // ── Dynamic view: media-heavy folders auto-switch to icon view once ──
  // Mirrors Dolphin's applyDynamicView: images/videos must outweigh all other
  // entries 2:1, with each subdirectory only counting a third.
  {
    bool searching = app.search_active || app.recursive_search_active ||
                     app.r_search_active || app.r_recursive_search_active;
    auto& tab = app.cur_tab();
    if (app.dynamic_view && !searching && tab.view_mode == ViewMode::List &&
        !tab.dynamic_view_done && !tab.entries.empty()) {
      int media = 0, dirs = 0, others = 0;
      std::string first_media;
      for (const auto& e : tab.entries) {
        if (!app.show_hidden && e.is_hidden) continue;
        if (e.type == FileType::Image || e.type == FileType::Video) {
          ++media;
          if (first_media.empty()) first_media = e.path;
        } else if (e.is_dir) {
          ++dirs;
        } else {
          ++others;
        }
      }
      double non_media_weight = static_cast<double>(others) +
                                static_cast<double>(dirs) / 3.0;
      if (media > 0 &&
          static_cast<double>(media) >= 2.0 * non_media_weight) {
        tab.view_mode = ViewMode::Grid;
        app.last_browser_view_mode = ViewMode::Grid;
        tab.dynamic_view_done = true;
        app.media_folder_child[tab.current_path] = first_media;
      }
    }
  }

  // Clear pending thumbnail queue since entry list changed
  app.thumb_pending_queue.clear();

  // Populate background pre-cache queue with all image file paths.
  // This ensures every image thumbnail is generated in the background
  // regardless of whether it's currently visible.
  app.precache_paths.clear();
  for (const auto& e : app.cur_tab().entries) {
    if (e.type == FileType::Image || e.type == FileType::Video) {
      app.precache_paths.push_back(e.path);
    }
  }
  app.precache_idx = 0;

  // Rebuild tree entries if in tree mode
  if (app.cur_tab().view_mode == ViewMode::Tree)
    build_tree_entries(app);
}

std::string format_mode(uint32_t mode) {
  std::string s = "---------";
  if (mode & 0400) s[0] = 'r';
  if (mode & 0200) s[1] = 'w';
  if (mode & 0100) s[2] = 'x';
  if (mode & 0040) s[3] = 'r';
  if (mode & 0020) s[4] = 'w';
  if (mode & 0010) s[5] = 'x';
  if (mode & 0004) s[6] = 'r';
  if (mode & 0002) s[7] = 'w';
  if (mode & 0001) s[8] = 'x';
  return s;
}

std::string group_label_for(const AppState& app, const FileEntry& e) {
  switch (app.cur_tab().group_field) {
    case 1:
      switch (e.type) {
        case FileType::Folder:     return "Folders";
        case FileType::Image:      return "Images";
        case FileType::Audio:      return "Audio";
        case FileType::Video:      return "Videos";
        case FileType::Text:       return "Text";
        case FileType::Markdown:   return "Markdown";
        case FileType::Code:       return "Code Files";
        case FileType::Document:   return "Documents";
        case FileType::Font:       return "Fonts";
        case FileType::Archive:    return "Archives";
        case FileType::Executable: return "Executables";
        case FileType::Web:        return "Web";
        default:                   return "Other Files";
      }
    case 2: {
      if (e.name.empty()) return "#";
      unsigned char c0 = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(e.name[0])));
      if (c0 >= 'a' && c0 <= 'z') {
        std::string s(1, static_cast<char>(std::toupper(c0)));
        return s;
      }
      return "#";
    }
    case 3: {
      std::time_t now = std::time(nullptr);
      double age_s = difftime(now, e.modified_sec);
      if (age_s < 0) age_s = 0;
      if (age_s < 86400) return "Today";
      if (age_s < 172800) return "Yesterday";
      if (age_s < 7 * 86400) return "This Week";
      if (age_s < 30 * 86400) return "This Month";
      if (age_s < 365 * 86400) return "This Year";
      return "Earlier";
    }
    case 4:
      if (e.is_dir) return "Folders";
      if (e.size < 100ull * 1024) return "Small (<100 KB)";
      if (e.size < 10ull * 1024 * 1024) return "Medium (<10 MB)";
      if (e.size < 1024ull * 1024 * 1024) return "Large (<1 GB)";
      return "Huge (≥1 GB)";
    default: return "";
  }
}

void clear_thumb_cache(AppState& app) {
  for (auto& [_, s] : app.thumb_cache) {
    if (s) cairo_surface_destroy(s);
  }
  app.thumb_cache.clear();
  app.thumb_lru.clear();
  app.thumb_cache_bytes = 0;
  app.thumb_pending_queue.clear();
}

// ── navigation ───────────────────────────────────────────────────

// Snapshot current view state into DirProps for per-folder persistence
static void save_dir_props_before_leave(AppState& app) {
  if (!app.per_folder_props) return;
  const std::string old_dir = app.cur_tab().current_path;
  if (old_dir.empty() || old_dir == "computer://" || old_dir == "trash://" ||
      old_dir.rfind("recent://", 0) == 0)
    return;

  DirProps p;
  p.has_mode = true;
  p.mode = static_cast<int>(app.cur_tab().view_mode);
  if (p.mode == static_cast<int>(ViewMode::Computer)) return;
  p.has_sort = true;
  p.sort_field = static_cast<int>(app.cur_tab().sort_field);
  p.sort_descending = app.cur_tab().sort_descending;
  p.has_flags = true;
  p.natural = app.sort_natural;
  p.case_sensitive = app.sort_case_sensitive;
  p.hidden_last = app.sort_hidden_last;
  p.folders_first = app.folders_before_files;
  p.has_group = true;
  p.group_field = app.cur_tab().group_field;
  p.has_zoom = true;
  p.zoom_level = zoom_level_for_pct(app.zoom_pct);
  p.has_hidden = true;
  p.show_hidden = app.show_hidden;

  write_dir_props(old_dir, p);
}

void navigate_to(AppState& app, const std::string& path) {
  save_dir_props_before_leave(app);
  // Handle virtual "computer://" path
  if (path == "computer://") {
    if (!app.cur_tab().current_path.empty() && app.cur_tab().current_path != path) {
      if (app.cur_tab().view_mode != ViewMode::Computer)
        app.last_browser_view_mode = app.cur_tab().view_mode;
      app.cur_tab().nav_history.push_back(app.cur_tab().current_path);
      app.cur_tab().nav_forward.clear();
    }
    app.cur_tab().current_path = path;
    app.cur_tab().view_mode = ViewMode::Computer;
    app.cur_tab().selected_idx = -1;
    app.cur_tab().hover_idx = -1;
    app.cur_tab().scroll_px = 0;
    app.cur_tab().scroll_smooth_current = 0;
    app.cur_tab().scroll_smooth_target = 0;
    app.computer_scroll_px = 0;
    app.computer_scroll_smooth_current = 0;
    app.computer_scroll_smooth_target = 0;
    app.computer_needs_refresh = true;
    draw(app);
    return;
  }

  std::string resolved = fs::absolute(path).string();
  if (!fs::is_directory(resolved)) return;

  // Push current folder onto the back stack before leaving
  if (!app.cur_tab().current_path.empty() && app.cur_tab().current_path != resolved) {
    app.cur_tab().nav_history.push_back(app.cur_tab().current_path);
    app.cur_tab().nav_forward.clear();
  }
  app.cur_tab().current_path = resolved;
  app.cur_tab().selected_idx = -1;
  app.cur_tab().hover_idx = -1;
  app.cur_tab().scroll_px = 0;
  app.cur_tab().scroll_smooth_current = 0;
  app.cur_tab().scroll_smooth_target = 0;

  // View/sort settings are global — keep the current ones across folders.
  // Only recover if we were in the virtual computer view.
  if (app.cur_tab().view_mode == ViewMode::Computer) {
    app.cur_tab().view_mode = app.last_browser_view_mode;
  }

  reload_dir(app);
  draw(app);
}

void navigate_up(AppState& app) {
  if (app.cur_tab().current_path == "computer://") return;
  fs::path p(app.cur_tab().current_path);
  auto parent = p.parent_path();
  if (parent != p) {
    navigate_to(app, parent.string());
  }
}

void navigate_back(AppState& app) {
  if (app.cur_tab().nav_history.empty()) return;
  save_dir_props_before_leave(app);
  app.cur_tab().nav_forward.push_back(app.cur_tab().current_path);
  app.cur_tab().current_path = app.cur_tab().nav_history.back();
  app.cur_tab().nav_history.pop_back();
  app.cur_tab().selected_idx = -1;
  app.cur_tab().scroll_px = 0;
  reload_dir(app);
  draw(app);
}

void navigate_forward(AppState& app) {
  if (app.cur_tab().nav_forward.empty()) return;
  save_dir_props_before_leave(app);
  app.cur_tab().nav_history.push_back(app.cur_tab().current_path);
  app.cur_tab().current_path = app.cur_tab().nav_forward.back();
  app.cur_tab().nav_forward.pop_back();
  app.cur_tab().selected_idx = -1;
  app.cur_tab().scroll_px = 0;
  reload_dir(app);
  draw(app);
}

// ── sidebar refresh ──────────────────────────────────────────────

void refresh_sidebar(AppState& app) {
  app.sidebar_locations.clear();
  std::string home = home_dir();

  auto add_location = [&](SidebarLocation::Kind kind, const char* label,
                           const std::string& path, const char* icon) {
    SidebarLocation loc;
    loc.kind = kind;
    loc.label = label;
    loc.path = path;
    loc.icon_name = icon;
    app.sidebar_locations.push_back(std::move(loc));
  };

  add_location(SidebarLocation::Kind::Computer, "My Computer", "computer://", "computer");

  add_location(SidebarLocation::Kind::Home, "Home", home, "user-home");
  add_location(SidebarLocation::Kind::Desktop, "Desktop", desktop_dir(), "user-desktop");
  add_location(SidebarLocation::Kind::Documents, "Documents",
               xdg_user_dir("XDG_DOCUMENTS_DIR", "Documents"), "folder-documents");
  add_location(SidebarLocation::Kind::Downloads, "Downloads",
               xdg_user_dir("XDG_DOWNLOAD_DIR", "Downloads"), "folder-download");
  add_location(SidebarLocation::Kind::Pictures, "Pictures",
               xdg_user_dir("XDG_PICTURES_DIR", "Pictures"), "folder-pictures");
  add_location(SidebarLocation::Kind::Music, "Music",
               xdg_user_dir("XDG_MUSIC_DIR", "Music"), "folder-music");
  add_location(SidebarLocation::Kind::Videos, "Videos",
               xdg_user_dir("XDG_VIDEOS_DIR", "Videos"), "folder-videos");
  add_location(SidebarLocation::Kind::Trash, "Trash",
               home + "/.local/share/Trash/files", "user-trash");

  // ── Favorites ──
  for (const auto& fav_path : app.favorites) {
    std::string label = fs::path(fav_path).filename().string();
    if (label.empty()) label = fav_path;
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Favorite;
    loc.label = label;
    loc.path = fav_path;
    loc.icon_name = "folder";
    app.sidebar_locations.push_back(std::move(loc));
  }

  // ── Drives ──

  // Build device → mountpoint map from /proc/mounts (needed for root label + drive filtering)
  std::map<std::string, std::string> mount_map;
  {
    auto* f = setmntent("/proc/mounts", "r");
    if (f) {
      struct mntent* mnt;
      while ((mnt = getmntent(f)) != nullptr) {
        std::string dev = mnt->mnt_fsname;
        if (dev.size() >= 5 && dev.substr(0, 5) == "/dev/")
          mount_map[dev] = mnt->mnt_dir;
      }
      endmntent(f);
    }
  }

  // Build a map of device path → UDisks2 info (if available).
  // This gives us drive_id for mount/unmount and accurate mount status.
  auto& udisks = drives::UDisks2DriveService::instance();
  auto udisks_drives = udisks.query_drives();
  std::map<std::string, const drives::DriveInfo*> udisk_map;
  for (const auto& d : udisks_drives)
    udisk_map[d.device] = &d;

  std::set<std::string> seen_devs;
  std::vector<SidebarLocation> drive_locs;

  auto add_drive = [&](const std::string& label, const std::string& dev) {
    if (!seen_devs.insert(dev).second) return;

    // Determine mount point and fs type for filtering
    std::string mp;
    std::string fs;
    auto ui = udisk_map.find(dev);
    if (ui != udisk_map.end()) {
      mp = ui->second->mounted ? ui->second->mount_point : "";
      fs = ui->second->id_type;
    } else {
      auto mi = mount_map.find(dev);
      if (mi != mount_map.end()) mp = mi->second;
    }

    if (drives::should_hide_drive(dev, mp, fs, label)) return;

    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Drive;
    loc.label = label;
    loc.icon_name = "drive-harddisk";

    if (ui != udisk_map.end()) {
      loc.drive_id = ui->second->object_path;
      loc.is_mounted = ui->second->mounted;
      loc.path = ui->second->mounted ? ui->second->mount_point : dev;
    } else {
      auto mi = mount_map.find(dev);
      loc.drive_id = dev;
      loc.is_mounted = (mi != mount_map.end());
      loc.path = loc.is_mounted ? mi->second : dev;
    }
    drive_locs.push_back(std::move(loc));
  };

  auto unescape_name = [](std::string s) {
    for (auto p = s.find("\\x20"); p != std::string::npos;
         p = s.find("\\x20", p + 1))
      s.replace(p, 4, " ");
    return s;
  };

  auto resolve_dev = [](const std::string& link_path) -> std::string {
    char buf[256];
    ssize_t len = readlink(link_path.c_str(), buf, sizeof(buf) - 1);
    if (len < 0) return {};
    buf[len] = '\0';
    std::string dev = buf;
    if (dev.size() > 6 && dev.substr(0, 6) == "../../")
      return "/dev/" + dev.substr(6);
    if (dev.size() > 3 && dev.substr(0, 3) == "../")
      return "/dev/" + dev.substr(3);
    return dev;
  };

  // Find root device and its filesystem label
  std::string root_dev;
  std::string root_label;
  for (auto& [dev, mp] : mount_map) {
    if (mp == "/") { root_dev = dev; break; }
  }
  if (!root_dev.empty()) {
    // Try filesystem label on the root device itself
    auto* dlabel = opendir("/dev/disk/by-label");
    if (dlabel) {
      struct dirent* entry;
      while ((entry = readdir(dlabel)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string link = resolve_dev(std::string("/dev/disk/by-label/") + entry->d_name);
        if (link == root_dev) {
          root_label = unescape_name(entry->d_name);
          break;
        }
      }
      closedir(dlabel);
    }
    // If no label on root device, try any partition on the same disk
    if (root_label.empty()) {
      std::string root_disk = drives::disk_device_from_partition(root_dev);
      auto* dlabel = opendir("/dev/disk/by-label");
      if (dlabel) {
        struct dirent* entry;
        while ((entry = readdir(dlabel)) != nullptr) {
          if (entry->d_name[0] == '.') continue;
          std::string link = resolve_dev(std::string("/dev/disk/by-label/") + entry->d_name);
          if (drives::disk_device_from_partition(link) == root_disk) {
            root_label = unescape_name(entry->d_name);
            break;
          }
        }
        closedir(dlabel);
      }
    }
    if (root_label.empty()) {
      // Try partition label from by-partlabel
      auto* plabel = opendir("/dev/disk/by-partlabel");
      if (plabel) {
        struct dirent* entry;
        while ((entry = readdir(plabel)) != nullptr) {
          if (entry->d_name[0] == '.') continue;
          std::string link = resolve_dev(std::string("/dev/disk/by-partlabel/") + entry->d_name);
          if (link == root_dev) {
            root_label = unescape_name(entry->d_name);
            break;
          }
        }
        closedir(plabel);
      }
    }
    if (root_label.empty() || drives::is_generic_partition_label(root_label)) {
      uint64_t size = drives::get_device_size_bytes(root_dev);
      if (size > 0)
        root_label = drives::format_device_size(size);
      else if (root_label.empty())
        root_label = root_dev.substr(5);
    }
  }
  add_location(SidebarLocation::Kind::Root,
               root_label.empty() ? "File System" : root_label.c_str(),
               "/", "drive-harddisk");
  {
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
      auto& root = app.sidebar_locations.back();
      root.total_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_blocks;
      root.free_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_bavail;
    }
  }

  // Exclude root device from drive scans (it's already shown as the Root entry above)
  if (!root_dev.empty()) seen_devs.insert(root_dev);

  // Scan /dev/disk/by-label/ for filesystem labels
  auto* d = opendir("/dev/disk/by-label");
  if (d) {
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      if (entry->d_name[0] == '.') continue;
      std::string label = unescape_name(entry->d_name);
      std::string dev = resolve_dev(std::string("/dev/disk/by-label/") + entry->d_name);
      if (dev.empty()) continue;
      add_drive(label, dev);
    }
    closedir(d);
  }

  // Scan /dev/disk/by-partlabel/ for GPT partition names not already seen
  d = opendir("/dev/disk/by-partlabel");
  if (d) {
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      if (entry->d_name[0] == '.') continue;
      std::string part_label = unescape_name(entry->d_name);
      std::string dev = resolve_dev(std::string("/dev/disk/by-partlabel/") + entry->d_name);
      if (dev.empty()) continue;
      if (drives::should_hide_drive(dev, {}, {}, part_label)) continue;
      std::string label;
      if (drives::is_generic_partition_label(part_label)) {
        uint64_t size = drives::get_device_size_bytes(dev);
        label = size > 0 ? drives::format_device_size(size) : part_label;
      } else {
        label = part_label;
      }
      add_drive(label, dev);
    }
    closedir(d);
  }

  // Also add any UDisks2 drives that weren't found by the scanning above
  for (const auto& d : udisks_drives) {
    if (seen_devs.count(d.device)) continue;
    std::string label = d.label;
    if (label == d.device.substr(d.device.find_last_of('/') + 1))
      label.clear();
    if (label.empty() || drives::is_generic_partition_label(label)) {
      // Try partition label from by-partlabel
      auto* plabel = opendir("/dev/disk/by-partlabel");
      if (plabel) {
        struct dirent* entry;
        while ((entry = readdir(plabel)) != nullptr) {
          if (entry->d_name[0] == '.') continue;
          std::string link = resolve_dev(std::string("/dev/disk/by-partlabel/") + entry->d_name);
          if (link == d.device) {
            std::string pl = unescape_name(entry->d_name);
            if (!drives::is_generic_partition_label(pl)) {
              label = pl;
            }
            break;
          }
        }
        closedir(plabel);
      }
    }
    if (label.empty()) {
      uint64_t size = drives::get_device_size_bytes(d.device);
      label = size > 0 ? drives::format_device_size(size) : d.device;
    }
    add_drive(label, d.device);
  }

  // Sort drives: mounted first, then unmounted
  std::stable_partition(drive_locs.begin(), drive_locs.end(),
                         [](const SidebarLocation& l) { return l.is_mounted; });

  // Query disk usage for mounted drives
  for (auto& loc : drive_locs) {
    if (loc.is_mounted && !loc.path.empty()) {
      struct statvfs vfs;
      if (statvfs(loc.path.c_str(), &vfs) == 0) {
        loc.total_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_blocks;
        loc.free_bytes = static_cast<uint64_t>(vfs.f_frsize) * vfs.f_bavail;
      }
    }
  }

  for (auto& loc : drive_locs)
    app.sidebar_locations.push_back(std::move(loc));
}

// ── mount drive ──────────────────────────────────────────────────

void mount_drive(AppState& app, int sb_idx) {
  auto& loc = app.sidebar_locations[sb_idx];
  if (loc.kind != SidebarLocation::Kind::Drive || loc.is_mounted) return;
  if (loc.drive_id.empty()) return; // no UDisks2 object path — can't mount

  auto& udisks = drives::UDisks2DriveService::instance();
  {
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    app.mount_pending_drive_id = loc.drive_id;
  }

  udisks.mount_async(loc.drive_id, [&app](bool ok) {
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    if (app.mount_pending_drive_id.empty()) return;
    app.mount_success = ok;
    app.mount_result_drive_id = std::move(app.mount_pending_drive_id);
    app.mount_pending_drive_id.clear();
    app.mount_poll_wake.store(true, std::memory_order_release);
  });
}

// ── unmount drive ────────────────────────────────────────────────

void unmount_drive(AppState& app, int sb_idx) {
  auto& loc = app.sidebar_locations[sb_idx];
  if (loc.kind != SidebarLocation::Kind::Drive || !loc.is_mounted) return;
  if (loc.drive_id.empty()) return;

  auto& udisks = drives::UDisks2DriveService::instance();
  {
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    app.unmount_pending_drive_id = loc.drive_id;
  }

  udisks.unmount_async(loc.drive_id, [&app](bool ok) {
    std::lock_guard<std::mutex> lock(app.mount_mtx);
    if (app.unmount_pending_drive_id.empty()) return;
    app.unmount_success = ok;
    app.unmount_result_drive_id = std::move(app.unmount_pending_drive_id);
    app.unmount_pending_drive_id.clear();
    app.mount_poll_wake.store(true, std::memory_order_release);
  });
}

// ── open files ───────────────────────────────────────────────────

void open_selected(AppState& app) {
  auto& tab = app.cur_tab();
  // Tree view: selected_idx is into tree_entries
  if (tab.view_mode == ViewMode::Tree) {
    if (tab.selected_idx < 0 || tab.selected_idx >= static_cast<int>(tab.tree_entries.size())) return;
    auto& te = tab.tree_entries[tab.selected_idx];
    std::error_code ec;
    if (fs::is_directory(te.path, ec)) {
      navigate_to(app, te.path);
    } else {
      xdg::open_path_in_default_application(te.path);
    }
    return;
  }
  if (tab.selected_idx < 0 ||
      tab.selected_idx >= static_cast<int>(tab.visible_entries.size()))
    return;

  int real_idx = tab.visible_entries[tab.selected_idx];
  if (real_idx < 0 || real_idx >= static_cast<int>(tab.entries.size())) return;

  auto& entry = app.cur_tab().entries[real_idx];
  if (entry.type == FileType::Archive) {
    pid_t pid = fork();
    if (pid == 0) {
      execlp("horizon-archive", "horizon-archive", entry.path.c_str(), nullptr);
      _exit(1);
    }
    return;
  }
  if (entry.is_dir) {
    navigate_to(app, entry.path);
  } else if (entry.type == FileType::Executable) {
    pid_t pid = fork();
    if (pid == 0) {
      setsid();
      execlp(entry.path.c_str(), entry.path.c_str(), nullptr);
      _exit(1);
    }
  } else {
    xdg::open_path_in_default_application(entry.path);
  }
}

// ── hover preview helper ─────────────────────────────────────────

// Forward declarations for preview popup helpers (defined below)
static void destroy_preview_popup(AppState& app);
static bool create_preview_popup(AppState& app);
static void commit_preview_popup(AppState& app);

// Aspect-fit the hover popup around a full-res frame (same math as images)
static void size_hover_popup_to_frame(int& popup_w, int& popup_h,
                                      int tw, int th) {
  int max_w = 700;
  int max_h = 800;
  int margin = 12;        // whitespace around image within image area
  int bottom_h = 50;      // filename + info area
  double scale = std::min({static_cast<double>(max_w - margin * 2) / tw,
                          static_cast<double>(max_h - margin * 2 - bottom_h) / th, 1.0});
  popup_w = std::max(150, static_cast<int>(tw * scale + margin * 2));
  popup_h = std::max(150, static_cast<int>(th * scale + margin * 2 + bottom_h));
}

// Position the hover popup centered on mouse X, above mouse Y, clamped on screen
static void place_hover_popup(AppState& app, int mx, int my,
                              int popup_w, int popup_h) {
  int popup_x = mx - popup_w / 2;
  int popup_y = my - popup_h - 20; // above cursor with 20px gap

  int content_x = app.sidebar_expanded ? app.sidebar_width : 0;
  int content_y = app.top_bar_height + app.tab_bar_height;
  if (popup_x < content_x + 10) popup_x = content_x + 10;
  if (popup_x + popup_w > app.width - 10)
    popup_x = app.width - popup_w - 10;
  if (popup_y < content_y + 10)          // above viewport → flip below
    popup_y = my + 20;
  if (popup_y + popup_h > app.height - app.status_bar_height - 10)
    popup_y = app.height - app.status_bar_height - popup_h - 10;

  app.preview_x = popup_x;
  app.preview_y = popup_y;
  app.preview_w = popup_w;
  app.preview_h = popup_h;
}

void reset_preview(AppState& app) {
  // Clean up popup surface if it exists
  destroy_preview_popup(app);
  app.preview_active = false;
  app.preview_mode = AppState::PreviewMode::None;
  app.preview_entry_idx = -1;
  app.preview_path.clear();
  app.preview_text.clear();
  app.preview_hover_start_ns = 0;
  if (app.preview_thumb) {
    cairo_surface_destroy(app.preview_thumb);
    app.preview_thumb = nullptr;
  }
}

void check_hover_preview(AppState& app) {
  // Don't touch space preview
  if (app.preview_mode == AppState::PreviewMode::Space) return;

  // Guard: if index is stale (entries changed), reset
  int n_visible = static_cast<int>(app.cur_tab().visible_entries.size());
  if (app.preview_entry_idx >= n_visible) {
    reset_preview(app);
    return;
  }
  if (app.preview_entry_idx < 0 && !app.preview_active) return;

  // If the entry is no longer hovered, cancel
  if (app.preview_entry_idx >= 0 && app.preview_entry_idx != app.cur_tab().hover_idx) {
    reset_preview(app);
    return;
  }

  // Timer: activate after 400ms
  if (!app.preview_active && app.preview_entry_idx >= 0) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                   static_cast<uint64_t>(ts.tv_nsec);
    if (now - app.preview_hover_start_ns > 400000000) { // 400ms
      int vi = app.preview_entry_idx;
      if (vi >= 0 && vi < static_cast<int>(app.cur_tab().visible_entries.size())) {
        int ri = app.cur_tab().visible_entries[vi];
        if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
          const auto& entry = app.cur_tab().entries[ri];

          int mx = static_cast<int>(app.pointerX);
          int my = static_cast<int>(app.pointerY);

          // Load thumbnail first so we can size the popup around it
          if (entry.type == FileType::Image) {
            if (is_svg_extension(entry.path))
              app.preview_thumb = load_svg_thumbnail(entry.path, 500);
            else
              app.preview_thumb = load_image_thumbnail(entry.path, 500);
            if (app.preview_thumb)
              cairo_surface_reference(app.preview_thumb);
            else
              preview_log("hover_preview: IMAGE thumb FAIL path=%s", entry.path.c_str());
          } else if (entry.type == FileType::Video) {
            // Pull a full-res frame from the video (async), like image decode;
            // popup upgrades to it when the extraction lands
            video_worker().enqueue_preview(entry.path, kVideoPreviewFrameMaxPx);
          } else if (entry.type == FileType::Audio) {
            app.preview_thumb = get_thumbnail(app, entry.path, 256);
            if (app.preview_thumb)
              cairo_surface_reference(app.preview_thumb);
            else
              preview_log("hover_preview: AUDIO thumb FAIL path=%s", entry.path.c_str());
          }

          // PDF gets a rendered thumbnail via poppler
          if (entry.type == FileType::Document && is_pdf_extension(entry.path)) {
            app.preview_thumb = load_pdf_thumbnail(entry.path, 256);
            if (app.preview_thumb)
              cairo_surface_reference(app.preview_thumb);
            else
              preview_log("hover_preview: PDF thumb FAIL path=%s", entry.path.c_str());
          }

          // EPUB gets a cover thumbnail via libarchive + stb_image
          if (entry.type == FileType::Document && is_epub_extension(entry.path)) {
            app.preview_thumb = load_epub_thumbnail(entry.path, 256);
            if (app.preview_thumb)
              cairo_surface_reference(app.preview_thumb);
            else
              preview_log("hover_preview: EPUB thumb FAIL path=%s", entry.path.c_str());
          }

          // Read text file content for preview (not for binary Document types like PDF/EPUB)
          if (entry.type == FileType::Text || entry.type == FileType::Markdown ||
              entry.type == FileType::Code) {
            FILE* f = fopen(entry.path.c_str(), "r");
            if (f) {
              char buf[513];
              size_t n = fread(buf, 1, 512, f);
              fclose(f);
              buf[n] = '\0';
              app.preview_text.assign(buf, n);
            }
          }

          // Compute popup size — images get aspect-ratio-sized popup
          int popup_w = 260;
          int popup_h = 240;
          if (entry.type == FileType::Image && app.preview_thumb) {
            int tw = cairo_image_surface_get_width(app.preview_thumb);
            int th = cairo_image_surface_get_height(app.preview_thumb);
            if (tw > 0 && th > 0)
              size_hover_popup_to_frame(popup_w, popup_h, tw, th);
          }

          place_hover_popup(app, mx, my, popup_w, popup_h);
          app.preview_path = entry.path;

          // Create + draw + commit the subsurface popup
          if (create_preview_popup(app))
            commit_preview_popup(app);

          app.preview_active = true;
          app.preview_mode = AppState::PreviewMode::Hover;
          app.pendingRedraw = true;
        }
      }
    }
  }

  // Re-try thumbnail for async decodes (video, etc.) while preview is active
  if (app.preview_active && app.preview_entry_idx >= 0) {
    int vi = app.preview_entry_idx;
    if (vi >= 0 && vi < static_cast<int>(app.cur_tab().visible_entries.size())) {
      int ri = app.cur_tab().visible_entries[vi];
      if (ri >= 0 && ri < static_cast<int>(app.cur_tab().entries.size())) {
        const auto& entry = app.cur_tab().entries[ri];
        if (entry.type == FileType::Video) {
          // Poll for the full-res frame extraction
          VideoThumbResult res;
          while (video_worker().poll_preview(res)) {
            if (res.path != app.preview_path) {
              // Stale result from an earlier hover
              if (res.surface) cairo_surface_destroy(res.surface);
              continue;
            }
            if (res.surface) {
              // Upgrade: swap in the full-res frame and resize the popup
              // around it, like images
              if (app.preview_thumb)
                cairo_surface_destroy(app.preview_thumb);
              app.preview_thumb = res.surface; // take ownership
              int tw = cairo_image_surface_get_width(app.preview_thumb);
              int th = cairo_image_surface_get_height(app.preview_thumb);
              if (tw > 0 && th > 0) {
                int popup_w = 260;
                int popup_h = 240;
                size_hover_popup_to_frame(popup_w, popup_h, tw, th);
                place_hover_popup(app, static_cast<int>(app.pointerX),
                                  static_cast<int>(app.pointerY),
                                  popup_w, popup_h);
                if (create_preview_popup(app))
                  commit_preview_popup(app);
              }
              app.pendingRedraw = true;
            } else if (!app.preview_thumb) {
              // Extraction failed — fall back to the low-res cached thumb
              cairo_surface_t* thumb = get_thumbnail(app, entry.path, 256);
              if (thumb) {
                cairo_surface_reference(thumb);
                app.preview_thumb = thumb;
                app.pendingRedraw = true;
              }
            }
            break;
          }
        } else if (!app.preview_thumb &&
                   (entry.type == FileType::Image || entry.type == FileType::Audio)) {
          cairo_surface_t* thumb = get_thumbnail(app, entry.path, 256);
          if (thumb) {
            cairo_surface_reference(thumb);
            app.preview_thumb = thumb;
            app.pendingRedraw = true;
          }
        } else if (entry.type == FileType::Document && is_pdf_extension(entry.path)) {
          cairo_surface_t* thumb = load_pdf_thumbnail(entry.path, 256);
          if (thumb) {
            cairo_surface_reference(thumb);
            app.preview_thumb = thumb;
            app.pendingRedraw = true;
          }
        } else if (entry.type == FileType::Document && is_epub_extension(entry.path)) {
          cairo_surface_t* thumb = load_epub_thumbnail(entry.path, 256);
          if (thumb) {
            cairo_surface_reference(thumb);
            app.preview_thumb = thumb;
            app.pendingRedraw = true;
          }
        }
      }
    }
  }
}

// ── Rich tooltip popup subsurface helpers ─────────────────────

static void destroy_tooltip_popup(AppState& app) {
  app.tooltipPopupBuf.destroy();
  if (app.tooltipPopupSub) {
    wl_subsurface_destroy(app.tooltipPopupSub);
    app.tooltipPopupSub = nullptr;
  }
  if (app.tooltipPopupSurface) {
    wl_surface_attach(app.tooltipPopupSurface, nullptr, 0, 0);
    wl_surface_commit(app.tooltipPopupSurface);
    wl_surface_destroy(app.tooltipPopupSurface);
    app.tooltipPopupSurface = nullptr;
  }
}

void hide_tooltip(AppState& app) {
  if (!app.tooltip_active && app.tooltip_path.empty() &&
      !app.tooltipPopupSurface)
    return;
  destroy_tooltip_popup(app);
  app.tooltip_active = false;
  app.tooltip_path.clear();
  app.tooltip_show_ms = 0;
}

static bool create_tooltip_popup(AppState& app) {
  destroy_tooltip_popup(app);

  if (app.tooltip_w < 1 || app.tooltip_h < 1) return false;

  wl_surface* surf = wl_compositor_create_surface(app.wl.compositor());
  if (!surf) return false;

  wl_subsurface* sub = wl_subcompositor_get_subsurface(app.wl.subcompositor(), surf, app.surface);
  if (!sub) {
    wl_surface_destroy(surf);
    return false;
  }

  int shadow_pad = 6;
  wl_subsurface_set_position(sub, app.tooltip_x - shadow_pad, app.tooltip_y - shadow_pad);
  wl_subsurface_place_above(sub, app.surface);

  app.tooltipPopupSurface = surf;
  app.tooltipPopupSub = sub;

  wl_surface_commit(app.tooltipPopupSurface);
  return true;
}

// Draw content is provided by draw.cpp; commit mirrors the preview popup.
void draw_tooltip_card(AppState& app, cairo_t* cr); // defined in draw.cpp

static void commit_tooltip_popup(AppState& app) {
  if (!app.tooltipPopupSurface || !app.tooltipPopupSub) return;

  int pw = app.tooltip_w;
  int ph = app.tooltip_h;
  if (pw < 1 || ph < 1) return;

  int shadow_pad = 6;
  int buf_w = pw + shadow_pad * 2;
  int buf_h = ph + shadow_pad * 2;

  app.tooltipPopupBuf.ensure(app.shm, eh::shell::kPopupNamespace, buf_w, buf_h);
  cairo_t* cr = app.tooltipPopupBuf.cairo();
  if (!cr) return;

  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_restore(cr);

  cairo_save(cr);
  cairo_translate(cr, -app.tooltip_x + shadow_pad, -app.tooltip_y + shadow_pad);
  draw_tooltip_card(app, cr);
  cairo_restore(cr);

  cairo_surface_flush(app.tooltipPopupBuf.cairo_surface());

  wl_surface_attach(app.tooltipPopupSurface, app.tooltipPopupBuf.wl(), 0, 0);
  wl_surface_damage_buffer(app.tooltipPopupSurface, 0, 0, buf_w, buf_h);
  app.tooltipPopupBuf.mark_busy();
  wl_surface_commit(app.tooltipPopupSurface);
  wl_display_flush(app.wl.display());
}

void check_hover_tooltip(AppState& app) {
  if (app.preview_mode == AppState::PreviewMode::Space) return;
  if (app.context_menu_open || app.preview_active) return;
  if (app.tooltip_active || app.tooltip_path.empty()) return;

  long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (now_ms < app.tooltip_show_ms) return;

  // Still hovering the armed entry?
  int vi = app.cur_tab().hover_idx;
  if (vi < 0 || vi >= static_cast<int>(app.cur_tab().visible_entries.size())) {
    hide_tooltip(app);
    return;
  }
  int ri = app.cur_tab().visible_entries[vi];
  if (ri < 0 || ri >= static_cast<int>(app.cur_tab().entries.size()) ||
      app.cur_tab().entries[ri].path != app.tooltip_path) {
    hide_tooltip(app);
    return;
  }
  const FileEntry& entry = app.cur_tab().entries[ri];

  // Metadata rows
  app.tooltip_title = entry.name;
  app.tooltip_rows.clear();
  auto fmt_size = [](uint64_t bytes) -> std::string {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024ull * 1024 * 1024)
      return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
  };
  const char* type_names[] = {"Folder", "Image", "Audio", "Video", "Text",
                              "Markdown", "Code", "Document", "Font",
                              "Archive", "Executable", "Web", "File"};
  {
    int ti = static_cast<int>(entry.type);
    std::string row = (ti >= 0 && ti < 13) ? type_names[ti] : "File";
    if (!entry.is_dir) row += "  \u00B7  " + fmt_size(entry.size);
    app.tooltip_rows.push_back(row);
  }
  if (entry.is_dir) {
    // Item count via bounded readdir
    long count = 0;
    if (DIR* d = opendir(entry.path.c_str())) {
      while (struct dirent* de = readdir(d)) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        if (++count > 9999) { count = 9999; break; }
      }
      closedir(d);
    }
    app.tooltip_rows.push_back(count >= 9999 ? "9999+ items"
                                             : std::to_string(count) + " items");
  }
  {
    char mb[64]{};
    struct tm tm_buf{};
    time_t mt = static_cast<time_t>(entry.modified_sec);
    if (localtime_r(&mt, &tm_buf))
      strftime(mb, sizeof(mb), "%Y-%m-%d %H:%M", &tm_buf);
    app.tooltip_rows.push_back(std::string("Modified ") + mb);
  }
  if (!entry.owner.empty())
    app.tooltip_rows.push_back(entry.owner + ":" + entry.group + "  " +
                               format_mode(entry.mode));

  // Size: title line + rows
  int card_w = 250;
  int row_h = 20;
  int pad = 12;
  int title_h = 24;
  int card_h = pad * 2 + title_h +
               static_cast<int>(app.tooltip_rows.size()) * row_h;
  app.tooltip_w = card_w;
  app.tooltip_h = card_h;

  // Position below-right of cursor, clamped to viewport
  {
    int mx = static_cast<int>(app.pointerX);
    int my = static_cast<int>(app.pointerY);
    int tx = mx + 16;
    int ty = my + 18;
    int content_x = app.sidebar_expanded ? app.sidebar_width : 0;
    int content_y = app.top_bar_height + app.tab_bar_height;
    if (tx + card_w > app.width - 10) tx = app.width - card_w - 10;
    if (tx < content_x + 10) tx = content_x + 10;
    if (ty + card_h > app.height - app.status_bar_height - 10)
      ty = my - card_h - 14;
    if (ty < content_y + 10) ty = content_y + 10;
    app.tooltip_x = tx;
    app.tooltip_y = ty;
  }

  if (create_tooltip_popup(app)) commit_tooltip_popup(app);
  app.tooltip_active = true;
  app.pendingRedraw = true;
}

// ── Preview popup subsurface helpers ──────────────────────────

static void destroy_preview_popup(AppState& app) {
  app.previewPopupBuf.destroy();
  if (app.previewPopupSub) {
    wl_subsurface_destroy(app.previewPopupSub);
    app.previewPopupSub = nullptr;
  }
  if (app.previewPopupSurface) {
    wl_surface_attach(app.previewPopupSurface, nullptr, 0, 0);
    wl_surface_commit(app.previewPopupSurface);
    wl_surface_destroy(app.previewPopupSurface);
    app.previewPopupSurface = nullptr;
  }
}

static bool create_preview_popup(AppState& app) {
  destroy_preview_popup(app);

  if (app.preview_w < 1 || app.preview_h < 1) return false;

  wl_surface* surf = wl_compositor_create_surface(app.wl.compositor());
  if (!surf) return false;

  wl_subsurface* sub = wl_subcompositor_get_subsurface(app.wl.subcompositor(), surf, app.surface);
  if (!sub) {
    wl_surface_destroy(surf);
    return false;
  }

  int shadow_pad = 6;
  wl_subsurface_set_position(sub, app.preview_x - shadow_pad, app.preview_y - shadow_pad);
  wl_subsurface_place_above(sub, app.surface);

  app.previewPopupSurface = surf;
  app.previewPopupSub = sub;

  wl_surface_commit(app.previewPopupSurface);
  return true;
}

static void commit_preview_popup(AppState& app) {
  if (!app.previewPopupSurface || !app.previewPopupSub) return;

  int pw = app.preview_w;
  int ph = app.preview_h;
  if (pw < 1 || ph < 1) return;

  int shadow_pad = 6;
  int buf_w = pw + shadow_pad * 2;
  int buf_h = ph + shadow_pad * 2;

  app.previewPopupBuf.ensure(app.shm, eh::shell::kPopupNamespace, buf_w, buf_h);
  cairo_t* cr = app.previewPopupBuf.cairo();
  if (!cr) return;

  // Clear to transparent
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_restore(cr);

  // Shift so the popup draws at (shadow_pad, shadow_pad) in the buffer,
  // leaving room for the drop shadow to extend right/down
  cairo_save(cr);
  cairo_translate(cr, -app.preview_x + shadow_pad, -app.preview_y + shadow_pad);
  draw_hover_preview(app, cr);
  cairo_restore(cr);

  cairo_surface_flush(app.previewPopupBuf.cairo_surface());

  wl_surface_attach(app.previewPopupSurface, app.previewPopupBuf.wl(), 0, 0);
  wl_surface_damage_buffer(app.previewPopupSurface, 0, 0, buf_w, buf_h);
  app.previewPopupBuf.mark_busy();
  wl_surface_commit(app.previewPopupSurface);
  wl_display_flush(app.wl.display());
}

// ── Space/Enter preview ─────────────────────────────────────────

void activate_space_preview(AppState& app) {
  int si = app.cur_tab().selected_idx;
  if (si < 0 || si >= static_cast<int>(app.cur_tab().visible_entries.size())) return;
  int ri = app.cur_tab().visible_entries[si];
  if (ri < 0 || ri >= static_cast<int>(app.cur_tab().entries.size())) return;
  const auto& entry = app.cur_tab().entries[ri];

  if (entry.is_dir) return;

  reset_preview(app);

  int thumb_px = 512;
  bool has_thumb = false;
  if (entry.type == FileType::Image) {
    if (is_svg_extension(entry.path))
      app.preview_thumb = load_svg_thumbnail(entry.path, thumb_px);
    else
      app.preview_thumb = load_image_thumbnail(entry.path, thumb_px);
    has_thumb = app.preview_thumb != nullptr;
    if (!has_thumb)
      preview_log("space_preview: IMAGE thumb FAIL path=%s", entry.path.c_str());
  } else if (entry.type == FileType::Video || entry.type == FileType::Audio) {
    app.preview_thumb = get_thumbnail(app, entry.path, thumb_px);
    has_thumb = app.preview_thumb != nullptr;
    if (!has_thumb)
      preview_log("space_preview: VIDEO/AUDIO thumb FAIL path=%s", entry.path.c_str());
  } else if (entry.type == FileType::Document) {
    if (is_pdf_extension(entry.path)) {
      app.preview_thumb = load_pdf_thumbnail(entry.path, thumb_px);
      has_thumb = app.preview_thumb != nullptr;
      if (!has_thumb)
        preview_log("space_preview: PDF thumb FAIL path=%s", entry.path.c_str());
    } else if (is_epub_extension(entry.path)) {
      app.preview_thumb = load_epub_thumbnail(entry.path, thumb_px);
      has_thumb = app.preview_thumb != nullptr;
      if (!has_thumb)
        preview_log("space_preview: EPUB thumb FAIL path=%s", entry.path.c_str());
    }
  }

  if (has_thumb)
    cairo_surface_reference(app.preview_thumb);

  if (entry.type == FileType::Text || entry.type == FileType::Markdown ||
      entry.type == FileType::Code) {
    FILE* f = fopen(entry.path.c_str(), "r");
    if (f) {
      char buf[2049];
      size_t n = fread(buf, 1, 2048, f);
      fclose(f);
      buf[n] = '\0';
      app.preview_text.assign(buf, n);
    }
  }

  // Popup sizing: fill proportionally within the viewport, centered
  int viewport_w = app.width;
  int viewport_h = app.height - app.status_bar_height;

  int max_w = std::clamp(viewport_w - 80, 300, 1000);
  int max_h = std::clamp(viewport_h - 80, 200, 900);
  int popup_w = max_w;
  int popup_h = max_h;

  if (has_thumb && app.preview_thumb) {
    int tw = cairo_image_surface_get_width(app.preview_thumb);
    int th = cairo_image_surface_get_height(app.preview_thumb);
    if (tw > 0 && th > 0) {
      int margin = 12;
      int bottom_h = 50;
      double scale = std::min(static_cast<double>(max_w - margin * 2) / tw,
                              static_cast<double>(max_h - margin * 2 - bottom_h) / th);
      popup_w = static_cast<int>(tw * scale + margin * 2);
      popup_h = static_cast<int>(th * scale + margin * 2 + bottom_h);
    }
  }

  int popup_x = (viewport_w - popup_w) / 2;
  int popup_y = (viewport_h - popup_h) / 2;
  if (popup_x < 10) popup_x = 10;
  if (popup_x + popup_w > viewport_w - 10) popup_x = viewport_w - popup_w - 10;
  if (popup_y < app.top_bar_height + app.tab_bar_height + 10)
    popup_y = app.top_bar_height + app.tab_bar_height + 10;
  if (popup_y + popup_h > viewport_h - 10)
    popup_y = viewport_h - popup_h - 10;

  app.preview_entry_idx = si;
  app.preview_path = entry.path;
  app.preview_x = popup_x;
  app.preview_y = popup_y;
  app.preview_w = popup_w;
  app.preview_h = popup_h;

  // Create + draw + commit the subsurface popup
  if (create_preview_popup(app))
    commit_preview_popup(app);

  app.preview_active = true;
  app.preview_mode = AppState::PreviewMode::Space;
}

void toggle_space_preview(AppState& app) {
  if (app.preview_mode == AppState::PreviewMode::Space) {
    reset_preview(app);
  } else {
    activate_space_preview(app);
  }
}

// ── lazy thumbnail processing + async search polling ────────────

bool process_pending_thumbnails(AppState& app) {
  // Drain completed async video thumbnails into the cache
  drain_video_thumbnails(app);

  // Poll recursive search results (both local and home-wide)
  bool search_is_active = (app.active_pane ? app.r_search_active : app.search_active) || (app.active_pane ? app.r_recursive_search_active : app.recursive_search_active);
  if (search_is_active && !(app.active_pane ? app.r_search_query : app.search_query).empty()) {
    bool got_any = false;
    SearchResult sr;
    // Cache of per-directory .hidden sets for the current result batch
    static std::unordered_map<std::string, std::unordered_set<std::string>> sr_hidden;
    sr_hidden.clear();
    while (recursive_search_worker().poll(sr)) {
      got_any = true;
      FileEntry entry;
      std::string filename = sr.path;
      auto slash = filename.rfind('/');
      entry.name = (slash != std::string::npos) ? filename.substr(slash + 1) : filename;
      entry.path = sr.path;
      entry.is_dir = sr.is_dir;
      entry.size = 0;
      if (!sr.is_dir) {
        entry.mime_type = mime_by_ext(entry.name);
      }
      entry.type = detect_file_type(entry.name, sr.is_dir, entry.mime_type);
      entry.is_hidden = is_hidden_file(entry.name);

      // Honor the parent directory's .hidden file
      std::string parent = (slash != std::string::npos) ? filename.substr(0, slash)
                                                        : std::string();
      if (!parent.empty()) {
        auto it = sr_hidden.find(parent);
        if (it == sr_hidden.end())
          it = sr_hidden.emplace(parent, read_hidden_file(parent)).first;
        if (it->second.count(entry.name) > 0) entry.is_hidden = true;
      }
      if (!app.show_hidden && entry.is_hidden) continue;

      // XDG user-directory icons from system theme
      if (entry.is_dir) {
        static const std::string xdg_home = home_dir();
        if      (entry.path == xdg_home + "/Desktop")    entry.icon_name = "user-desktop";
        else if (entry.path == xdg_home + "/Documents")  entry.icon_name = "folder-documents";
        else if (entry.path == xdg_home + "/Downloads")  entry.icon_name = "folder-download";
        else if (entry.path == xdg_home + "/Music")      entry.icon_name = "folder-music";
        else if (entry.path == xdg_home + "/Pictures")   entry.icon_name = "folder-pictures";
        else if (entry.path == xdg_home + "/Videos")     entry.icon_name = "folder-videos";
        else if (entry.path == xdg_home + "/Public")     entry.icon_name = "folder-publicshare";
        else if (entry.path == xdg_home + "/Templates")  entry.icon_name = "folder-templates";
      }

      // Parse .desktop file icon
      if (!entry.is_dir) {
        auto dot = entry.name.rfind('.');
        if (dot != std::string::npos) {
          std::string ext = entry.name.substr(dot + 1);
          for (auto& c : ext) c = std::tolower(c);
          if (ext == "desktop") {
            auto icon = parse_desktop_icon(entry.path);
            if (!icon.empty()) entry.icon_name = std::move(icon);
          }
        }
      }

      // Derive Freedesktop icon name from MIME type (icon themes use `media-subtype` format)
      if (entry.icon_name.empty() && !entry.mime_type.empty()) {
        std::string icon = entry.mime_type;
        for (auto& c : icon) if (c == '/') c = '-';
        entry.icon_name = std::move(icon);
      }

      // Populate size and modification time from stat for filter support
      struct stat st;
      if (::stat(entry.path.c_str(), &st) == 0) {
        entry.size = static_cast<uint64_t>(st.st_size);
        entry.modified_sec = st.st_mtime;
      }

      app.cur_tab().entries.push_back(std::move(entry));
    }
    if (got_any) {
      app.cur_tab().visible_entries.clear();
      bool has_filters = (app.active_pane ? app.r_filter_type_idx : app.filter_type_idx) > 0 || (app.active_pane ? app.r_filter_size_idx : app.filter_size_idx) > 0 || (app.active_pane ? app.r_filter_date_idx : app.filter_date_idx) > 0;
      for (int i = 0; i < static_cast<int>(app.cur_tab().entries.size()); ++i) {
        if (!has_filters || matches_filter(app, app.cur_tab().entries[i]))
          app.cur_tab().visible_entries.push_back(i);
      }
      app.cur_tab().selected_idx = 0;
      app.cur_tab().scroll_px = 0;
      app.pendingRedraw = true;
    }
  }

  // Process the visible-pending queue (lazy thumbnails from get_thumbnail_lazy)
  for (int i = 0; i < AppState::kThumbDecodesPerLoop; ++i) {
    if (app.thumb_pending_queue.empty()) break;

    auto pending = app.thumb_pending_queue.back();
    app.thumb_pending_queue.pop_back();

    int vi = pending.visible_idx;
    if (vi < 0 || vi >= static_cast<int>(app.cur_tab().visible_entries.size()))
      continue;

    int real_idx = app.cur_tab().visible_entries[vi];
    if (real_idx < 0 || real_idx >= static_cast<int>(app.cur_tab().entries.size()))
      continue;

    auto& entry = app.cur_tab().entries[real_idx];
    // Skip types that don't support thumbnails
    if (entry.type != FileType::Image && entry.type != FileType::Video) {
      if (entry.type != FileType::Document) continue;
      if (!is_pdf_extension(entry.path) && !is_epub_extension(entry.path)) continue;
    }
    if (app.thumb_cache.find(entry.path) != app.thumb_cache.end())
      continue;

    get_thumbnail(app, entry.path, pending.size);
  }

  // Process the background pre-cache queue (all images in the folder)
  if (app.precache_idx != SIZE_MAX) {
    for (int i = 0; i < AppState::kPrecacheBatchSize; ++i) {
      if (app.precache_idx >= app.precache_paths.size()) {
        app.precache_idx = SIZE_MAX;
        break;
      }
      const std::string& path = app.precache_paths[app.precache_idx++];
      if (app.thumb_cache.find(path) == app.thumb_cache.end()) {
        get_thumbnail(app, path, 128);
      }
    }
  }

  return !app.thumb_pending_queue.empty() || app.precache_idx != SIZE_MAX;
}

// ── Tab management ───────────────────────────────────────────────

void new_tab(AppState& app) {
  int idx = static_cast<int>(app.tabs.size());
  app.tabs.emplace_back();
  auto& prev = app.cur_tab();
  auto& next = app.tabs.back();
  next.view_mode = prev.view_mode;
  next.sort_field = prev.sort_field;
  next.sort_descending = prev.sort_descending;
  next.current_path = home_dir();
  app.active_tab = idx;
  navigate_to(app, next.current_path);
}

void close_tab(AppState& app) {
  if (app.tabs.size() <= 1) return;
  int idx = app.active_tab;
  remember_closed_tab(app, app.tabs[idx]);
  app.tabs.erase(app.tabs.begin() + idx);
  if (idx >= static_cast<int>(app.tabs.size()))
    app.active_tab = static_cast<int>(app.tabs.size()) - 1;
  reload_dir(app);
}

void next_tab(AppState& app) {
  if (app.tabs.size() <= 1) return;
  app.active_tab = (app.active_tab + 1) % static_cast<int>(app.tabs.size());
  reload_dir(app);
}

void prev_tab(AppState& app) {
  if (app.tabs.size() <= 1) return;
  app.active_tab = (app.active_tab - 1 + static_cast<int>(app.tabs.size())) %
                   static_cast<int>(app.tabs.size());
  reload_dir(app);
}

} // namespace eh::file_browser
