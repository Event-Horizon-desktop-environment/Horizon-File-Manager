// filetype.cpp
// Moved wholesale from features/nav.cpp (byte-identical bodies).


// Must come first: exposes struct statx through <sys/stat.h>.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "../../trace.hpp"

#include "../../app.hpp"
#include "app/file_browser/features/filetype/filetype.hpp"

#include <chrono>

#include "app/file_browser/features/desktop_icon_parser/desktop_icon_parser.hpp"
#include "app/file_browser/features/dirprops/dirprops.hpp"
#include "app/file_browser/features/query_match/query_match.hpp"
#include "app/file_browser/features/recursive_search_worker/recursive_search_worker.hpp"
#include "app/file_browser/features/tab_history/tab_history.hpp"
#include "app/file_browser/features/thumbnails/thumb_pool.hpp"



#include "app/file_browser/features/preview/video_worker.hpp"
#include "app/file_browser/features/preview/svg_preview.hpp"
#include "app/file_browser/features/preview/pdf_preview.hpp"
#include "app/file_browser/features/preview/epub_preview.hpp"
#include "app/file_browser/features/preview/image_preview.hpp"
#include "app/file_browser/features/view_zoom/view_zoom.hpp"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cmath>
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
#include <sys/syscall.h>
#if defined(EH_HAVE_IO_URING)
#include <liburing.h>
#endif

#include "services/udisks2/drive_filter.hpp"

#include <gio/gio.h>

#include "services/udisks2/udisks2_drive_service.hpp"

#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "platform/common/ns/namespaces.hpp"
#include "wayland/surface/layer_surface.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;


namespace eh::file_browser {
// Owner/group name lookup with caching to avoid repeated passwd/group hits.
// Sharded lookup caches so scan workers don't contend on one mutex.
static constexpr size_t kIdCacheShards = 16;

const std::string& user_name(uid_t uid) {
  static std::mutex mtx[kIdCacheShards];
  static std::unordered_map<uid_t, std::string> cache[kIdCacheShards];
  const size_t sh = uid % kIdCacheShards;
  {
    std::lock_guard<std::mutex> lk(mtx[sh]);
    auto it = cache[sh].find(uid);
    if (it != cache[sh].end()) return it->second;
  }
  // Resolve outside the lock; getpwuid is not thread-safe, use _r form.
  char buf[4096];
  struct passwd pw, *res = nullptr;
  getpwuid_r(uid, &pw, buf, sizeof(buf), &res);
  std::lock_guard<std::mutex> lk(mtx[sh]);
  return cache[sh]
      .emplace(uid, res ? res->pw_name : std::to_string(uid))
      .first->second;
}

const std::string& group_name(gid_t gid) {
  static std::mutex mtx[kIdCacheShards];
  static std::unordered_map<gid_t, std::string> cache[kIdCacheShards];
  const size_t sh = gid % kIdCacheShards;
  {
    std::lock_guard<std::mutex> lk(mtx[sh]);
    auto it = cache[sh].find(gid);
    if (it != cache[sh].end()) return it->second;
  }
  char buf[4096];
  struct group gr, *res = nullptr;
  getgrgid_r(gid, &gr, buf, sizeof(buf), &res);
  std::lock_guard<std::mutex> lk(mtx[sh]);
  return cache[sh]
      .emplace(gid, res ? res->gr_name : std::to_string(gid))
      .first->second;
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
    // Plain-text subtitle containers (SubRip etc.) preview as text.
    if (mime.find("subrip") != std::string::npos ||
        mime == "application/x-subtitle")
      return FileType::Text;

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

FileType detect_file_type(const std::string& name, bool is_dir,
                                  const std::string& mime_type,
                                  const std::string& full_path,
                                  const std::string& ext_hint) {
  if (is_dir) return FileType::Folder;

  // Fast path: extension-based detection
  std::string ext = ext_hint;
  if (ext.empty()) {
    auto dot = name.rfind('.');
    if (dot != std::string::npos && dot != name.size() - 1) {
      ext = name.substr(dot + 1);
      for (auto& c : ext) c = std::tolower(c);
    }
  }
  if (!ext.empty()) {

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
        ext == "sty" || ext == "bst" ||
        ext == "srt" || ext == "vtt" || ext == "ass" || ext == "ssa" ||
        ext == "sub")
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

  // Content-sniffing fallback for directory browsing (not search results).
  // Must never block: a FIFO with no writer (e.g. ~/.steam/steam.pipe) hangs
  // plain open(O_RDONLY) forever, which would freeze the directory scan. Open
  // non-blocking and only sniff regular files — pipes, sockets and devices
  // are skipped before any read.
  if (!full_path.empty()) {
    int fd = ::open(full_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
      struct stat st{};
      if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        unsigned char buf[512];
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
          gboolean uncertain = FALSE;
          gchar* ct =
              g_content_type_guess(name.c_str(), buf, n, &uncertain);
          if (ct) {
            std::string mime(ct);
            g_free(ct);
            FileType ft = mime_to_file_type(mime);
            if (ft != FileType::File) {
              ::close(fd);
              return ft;
            }
          }
        }
      }
      ::close(fd);
    }
  }

  // Old-style mime prefix fallback
  if (mime_type.size() > 6 && mime_type.substr(0, 6) == "image/")
    return FileType::Image;

  return FileType::File;
}

std::string mime_by_ext(const std::string& path) {
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
  if (ext == "wav") return "audio/x-wav";
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
  if (ext == "h") return "text/x-chdr";
  if (ext == "hpp" || ext == "hxx" || ext == "hh") return "text/x-c++hdr";
  if (ext == "py" || ext == "pyw") return "text/x-python";
  if (ext == "rs") return "text/x-rust";
  if (ext == "go") return "text/x-go";
  if (ext == "java") return "text/x-java";
  if (ext == "js") return "text/javascript";
  if (ext == "ts") return "text/x-typescript";
  if (ext == "jsx") return "text/javascript";
  if (ext == "tsx") return "text/x-typescript";
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
  if (ext == "srt") return "application/x-subrip";
  if (ext == "vtt") return "text/vtt";
  if (ext == "ass" || ext == "ssa") return "text/x-ssa";
  if (ext == "sub") return "text/x-microdvd";
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
  if (ext == "cbr" || ext == "cbz") return "application/x-cbr";
  if (ext == "doc") return "application/msword";
  if (ext == "docx")
    return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  if (ext == "xls") return "application/vnd.ms-excel";
  if (ext == "xlsx")
    return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  if (ext == "ppt") return "application/vnd.ms-powerpoint";
  if (ext == "pptx")
    return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
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
  if (ext == "iso") return "application/x-cd-image";
  if (ext == "cab") return "application/vnd.ms-cab-compressed";
  if (ext == "dmg") return "application/x-apple-diskimage";
  if (ext == "zoo") return "application/x-zoo";
  if (ext == "hqx") return "application/mac-binhex40";
  if (ext == "sit") return "application/x-stuffit";

  // ── Executables ─────────────────────────────────────────────────
  if (ext == "sh" || ext == "bash" || ext == "zsh") return "application/x-shellscript";
  if (ext == "bin") return "application/x-executable";
  if (ext == "elf") return "application/x-executable";
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
// Classify a filesystem entry for callers outside the scan pipeline (e.g.
// tree-view children read lazily in draw.cpp). Same detection chain as
// directory scans: extension fast path, then content sniff on full_path.
FileType detect_file_type_for_path(const std::string& name, bool is_dir,
                                   const std::string& full_path) {
  return detect_file_type(name, is_dir, mime_by_ext(name), full_path);
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

} // namespace eh::file_browser
