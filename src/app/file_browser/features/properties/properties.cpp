// properties.cpp — Exported from features/menu.cpp as part of the Step 5 file split.

#include "../../app.hpp"
#include "app/file_browser/features/compare/compare.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/dirprops/dirprops.hpp"
#include "app/file_browser/features/progress/progress.hpp"
#include "app/file_browser/features/selection/selection.hpp"
#include "app/file_browser/features/tab_history/tab_history.hpp"
#include "app/file_browser/features/tags/tags.hpp"
#include "app/file_browser/features/view_zoom/view_zoom.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <grp.h>
#include <pwd.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <thread>

#include "config/shell_config.hpp"
#include "base/thread/thread_dispatch.hpp"
#include "platform/common/palette/matugen_palette.hpp"
#include "platform/desktop/entries/desktop_xdg_ops.hpp"
#include "dialog/file_chooser_dialog.hpp"
#include "platform/widgets/app_drawer/list/desktop_list.hpp"

namespace fs = std::filesystem;
namespace xdg = eh::shell::desktop::xdg;
using menu_clock = std::chrono::steady_clock;

namespace eh::file_browser {

// Recursive file/dir/byte totals for one directory tree. Runs on a worker
// thread (see show_properties): never call on the UI thread for large trees.
// Never throws: every iterator step is error-code guarded and the whole walk
// is wrapped, so a tree that changes (or errors) mid-walk yields partial
// totals instead of a filesystem_error that would terminate the app from a
// detached worker.
DirSize walk_dir_size(const std::string& path) noexcept {
  DirSize r;
  try {
    std::error_code ec;
    fs::recursive_directory_iterator it(
        path, fs::directory_options::skip_permission_denied, ec);
    if (ec) return r;
    const fs::recursive_directory_iterator end;
    while (it != end) {
      try {
        std::error_code e2;
        if (it->is_regular_file(e2) && !e2) {
          ++r.files;
          struct stat fst;
          if (stat(it->path().c_str(), &fst) == 0)
            r.bytes += static_cast<uint64_t>(fst.st_size);
        } else if (it->is_directory(e2) && !e2) {
          ++r.dirs;
        }
      } catch (...) {
        // Entry vanished or errored between readdir and stat: skip it.
      }
      it.increment(ec);
      if (ec) break;  // iterator is end now; keep the partial totals
    }
  } catch (...) {
    // Never let a size probe take down the process; report partials.
  }
  return r;
}

// Shell-escape a path for single-quote quoting
static std::string sq_path(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  r += "'";
  return r;
}

static const std::vector<std::string> img_exts = {
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg", ".avif",
    ".tif", ".tiff", ".ico", ".heic", ".heif", ".psd", ".xcf", ".ai",
    ".eps", ".raw", ".cr2", ".nef", ".arw", ".dng", ".orf", ".raf",
    ".pbm", ".pgm", ".ppm", ".xbm", ".xpm", ".jxl", ".jp2", ".j2k",
    ".jpx", ".tga", ".dds", ".exr", ".hdr", ".cur", ".icns", ".qoi"
  };

// Image tab: dimensions/colorspace via identify, ffprobe fallback.
// local_path must be a real file (Drive callers download first).
static void probe_image_file(const std::string& local_path,
                             AppState::PropertiesState& p) {
  // First pass: dimensions via identify (first frame wins for GIF/TIFF).
  // %U carries the resolution unit so 72 DPI isn't misread as DPCM.
  std::string icmd = "identify -format '%w %h|%[colorspace]|%[bit-depth]|%A|%C|%x|%y|%U' " + sq_path(local_path) + " 2>/dev/null";
  FILE* f = popen(icmd.c_str(), "r");
  if (f) {
    char buf[1024];
    if (fgets(buf, sizeof(buf), f)) {
      std::string line(buf);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      // Parse pipe-delimited fields
      auto next_field = [&]() -> std::string {
        auto ppos = line.find('|');
        if (ppos == std::string::npos) { std::string r = line; line.clear(); return r; }
        std::string r = line.substr(0, ppos);
        line = line.substr(ppos + 1);
        return r;
      };
      std::string dims = next_field();
      {
        int w = 0, h = 0;
        if (sscanf(dims.c_str(), "%d %d", &w, &h) == 2) {
          p.image_w = (w > 0) ? w : 0;
          p.image_h = (h > 0) ? h : 0;
        }
      }
      p.image_colorspace = next_field();
      p.image_bit_depth = next_field();
      std::string alpha = next_field();
      // %A is Undefined/False/None/Off when there is no alpha channel;
      // Blend/True/Activate mean real transparency.
      p.image_has_alpha = (alpha == "True" || alpha == "Blend" || alpha == "Activate");
      p.image_compression = next_field();
      std::string res_x = next_field();
      std::string res_y = next_field();
      std::string res_unit = next_field();
      if (!res_x.empty() && !res_y.empty()) {
        try {
          double rx = std::stod(res_x);
          double ry = std::stod(res_y);
          if (rx > 0 && ry > 0) {
            char rbuf[32];
            snprintf(rbuf, sizeof(rbuf), "%.0f \u00d7 %.0f", rx, ry);
            p.image_resolution = rbuf;
            if (res_unit.find("Centimeter") != std::string::npos)
              p.image_res_unit = "DPCM";
            else
              p.image_res_unit = "DPI"; // PixelsPerInch or Undefined (SVG/WEBP default 96)
          }
        } catch (...) {}
      }
    }
    pclose(f);
  }
  // Fallback: try ffprobe for image dimensions
  if (p.image_w == 0 || p.image_h == 0) {
    std::string fcmd = "ffprobe -v quiet -print_format json -show_streams " + sq_path(local_path) + " 2>/dev/null";
    FILE* f2 = popen(fcmd.c_str(), "r");
    if (f2) {
      std::string out;
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf) - 1, f2)) > 0) { buf[n] = '\0'; out += buf; }
      pclose(f2);
      auto sfind = [&](const std::string& s, const std::string& key) -> std::string {
        auto kp = s.find("\"" + key + "\""); if (kp == std::string::npos) return {};
        auto cp = s.find(':', kp + key.size() + 2); if (cp == std::string::npos) return {};
        ++cp; while (cp < s.size() && (s[cp] == ' ' || s[cp] == '\t')) ++cp;
        if (cp >= s.size()) return {};
        if (s[cp] == '"') { ++cp; auto e = s.find('"', cp); if (e == std::string::npos) return {}; return s.substr(cp, e - cp); }
        auto e = s.find_first_of(",}\n\r", cp); if (e == std::string::npos) e = s.size();
        return s.substr(cp, e - cp);
      };
      auto sp = out.find("\"streams\"");
      if (sp != std::string::npos) {
        auto sobj = out.find('{', sp);
        if (sobj != std::string::npos) {
          auto sobj_end = out.find('}', sobj);
          if (sobj_end != std::string::npos) {
            std::string s = out.substr(sobj, sobj_end - sobj + 1);
            if (s.find("\"codec_type\"") != std::string::npos && sfind(s, "codec_type") == "video") {
              try {
                std::string vws = sfind(s, "width"); if (!vws.empty()) p.image_w = std::stoi(vws);
                std::string vhs = sfind(s, "height"); if (!vhs.empty()) p.image_h = std::stoi(vhs);
              } catch (...) {}
            }
          }
        }
      }
    }
  }
}

static const std::vector<std::string> media_exts = {
  ".mp3", ".flac", ".ogg", ".wav", ".aac", ".m4a", ".wma", ".opus", ".ac3", ".dsf", ".aiff",
  ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".webm", ".flv", ".m4v", ".ogv", ".3gp", ".mts", ".m2ts", ".ts"
};

static void probe_media_file(const std::string& local_path,
                             AppState::PropertiesState& p) {    std::string fcmd = "ffprobe -v quiet -print_format json -show_format -show_streams " + sq_path(local_path) + " 2>/dev/null";
    FILE* f = popen(fcmd.c_str(), "r");
    if (f) {
      std::string out;
      char buf[4096];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
        buf[n] = '\0';
        out += buf;
      }
      pclose(f);

      if (!out.empty()) {
        p.is_media = true;

        auto find_val = [&](const std::string& key) -> std::string {
          auto kp = out.find("\"" + key + "\"");
          if (kp == std::string::npos) return {};
          auto cp = out.find(':', kp + key.size() + 2);
          if (cp == std::string::npos) return {};
          ++cp;
          while (cp < out.size() && (out[cp] == ' ' || out[cp] == '\t')) ++cp;
          if (cp >= out.size()) return {};
          if (out[cp] == '"') {
            ++cp;
            auto e = out.find('"', cp);
            if (e == std::string::npos) return {};
            return out.substr(cp, e - cp);
          }
          auto e = out.find_first_of(",}\n\r", cp);
          if (e == std::string::npos) e = out.size();
          return out.substr(cp, e - cp);
        };

        auto sfind = [&](const std::string& s, const std::string& key) -> std::string {
          auto kp = s.find("\"" + key + "\"");
          if (kp == std::string::npos) return {};
          auto cp = s.find(':', kp + key.size() + 2);
          if (cp == std::string::npos) return {};
          ++cp;
          while (cp < s.size() && (s[cp] == ' ' || s[cp] == '\t')) ++cp;
          if (cp >= s.size()) return {};
          if (s[cp] == '"') { ++cp; auto e = s.find('"', cp); if (e == std::string::npos) return {}; return s.substr(cp, e - cp); }
          auto e = s.find_first_of(",}\n\r", cp);
          if (e == std::string::npos) e = s.size();
          return s.substr(cp, e - cp);
        };

        p.container = find_val("format_name");
        std::string dur_str = find_val("duration");
        if (!dur_str.empty()) {
          try { p.media_duration = std::stod(dur_str); } catch (...) {}
        }

        // Parse streams array
        auto sp = out.find("\"streams\"");
        if (sp != std::string::npos) {
          size_t pos = sp;
          while (true) {
            auto sobj = out.find('{', pos);
            if (sobj == std::string::npos) break;
            auto sobj_end = out.find('}', sobj);
            if (sobj_end == std::string::npos) break;
            std::string s = out.substr(sobj, sobj_end - sobj + 1);
            pos = sobj_end + 1;
            if (s.find("\"codec_type\"") == std::string::npos) continue;

            std::string ct = sfind(s, "codec_type");
            if (ct == "video") {
              p.has_video = true;
              p.video_codec = sfind(s, "codec_name");
              try {
                std::string vws = sfind(s, "width");
                if (!vws.empty()) p.video_w = std::stoi(vws);
                std::string vhs = sfind(s, "height");
                if (!vhs.empty()) p.video_h = std::stoi(vhs);
              } catch (...) {}
              std::string fr = sfind(s, "r_frame_rate");
              if (!fr.empty()) {
                auto sl = fr.find('/');
                if (sl != std::string::npos) {
                  try {
                    double num = std::stod(fr.substr(0, sl));
                    double den = std::stod(fr.substr(sl + 1));
                    if (den > 0) {
                      char fpb[16];
                      snprintf(fpb, sizeof(fpb), "%.2f", num / den);
                      p.video_framerate = fpb;
                    }
                  } catch (...) {}
                } else p.video_framerate = fr;
              }
              try {
                std::string vbr = sfind(s, "bit_rate");
                if (!vbr.empty()) p.video_bitrate = std::stoi(vbr);
              } catch (...) {}
            } else if (ct == "audio") {
              p.has_audio = true;
              p.audio_codec = sfind(s, "codec_name");
              try {
                std::string sr = sfind(s, "sample_rate");
                if (!sr.empty()) p.audio_sample_rate = std::stoi(sr);
                std::string ch = sfind(s, "channels");
                if (!ch.empty()) p.audio_channels = std::stoi(ch);
                std::string abr = sfind(s, "bit_rate");
                if (!abr.empty()) p.audio_bitrate = std::stoi(abr);
              } catch (...) {}
            }
          }
        }

        // Fallback: format-level bitrate (last "bit_rate" in JSON = format section)
        {
          auto extract_js_val = [&](size_t c) -> std::string {
            if (c >= out.size()) return {};
            if (out[c] == '"') { ++c; auto e = out.find('"', c); if (e == std::string::npos) return {}; return out.substr(c, e - c); }
            auto e = out.find_first_of(",}\n\r", c);
            if (e == std::string::npos) e = out.size();
            return out.substr(c, e - c);
          };
          auto fmt_br_pos = out.rfind("\"bit_rate\"");
          if (fmt_br_pos != std::string::npos) {
            auto cp = out.find(':', fmt_br_pos + 10);
            if (cp != std::string::npos) {
              ++cp;
              while (cp < out.size() && (out[cp] == ' ' || out[cp] == '\t')) ++cp;
              std::string fmt_br = extract_js_val(cp);
              if (!fmt_br.empty()) {
                try {
                  int fbr = std::stoi(fmt_br);
                  if (p.video_bitrate == 0 && p.has_video) p.video_bitrate = fbr;
                  if (p.audio_bitrate == 0 && p.has_audio) p.audio_bitrate = fbr;
                } catch (...) {}
              }
            }
          }
        }
      }
    }
}

// Drive-backed properties: everything comes from the Drive API on a worker
// thread; the dialog opens immediately with a pending state (like local
// dir sizing). Edits (permissions/tags/rating/comments) stay hidden for
// Drive items — see draw_properties and the apply-site guards.
static void show_drive_properties(AppState& app, const std::string& path,
                                  const std::string& icon_name,
                                  uint64_t gen) {
  auto& p = app.properties;
  p.drive_item = true;
  p.icon_name = icon_name;
  std::string email = drive_email(path);
  std::string dpath = drive_display_path(path);
  std::string raw = dpath;
  auto slash = raw.rfind('/');
  std::string name_raw =
      (slash == std::string::npos) ? raw : raw.substr(slash + 1);
  gchar* un = g_uri_unescape_string(name_raw.c_str(), nullptr);
  p.name = (un && *un) ? un : name_raw;
  g_free(un);
  if (p.name.empty()) p.name = email;
  p.location = drive_parent(path);
  p.dir_size_pending = true;
  create_props_window(app);

  AppState* ap = &app;
  std::thread([ap, path, email, gen]() {
    std::string err;
    DriveLoc loc =
        drive_locate(*ap, email, drive_display_path(path), err, [] { return false; });
    std::string access;
    DriveMeta meta;
    bool have_meta = false;
    if (!loc.id.empty() &&
        drive_ensure_token(*ap, email, access, err)) {
      if (loc.id == "root" || loc.id == "shared:roots" ||
          loc.id.rfind("shared:", 0) == 0) {
        have_meta = true;
        meta.exists = true;
        meta.is_dir = true;
        meta.mime = "inode/directory";
      } else {
        have_meta =
            drive_file_meta(access, loc.id, loc.drive_id, meta, err);
      }
    }
    DriveTreeSize tree;
    bool walked = false;
    if (have_meta && meta.is_dir && loc.id != "shared:roots") {
      std::string werr;
      walked = drive_folder_size(path, tree.files, tree.dirs, tree.bytes,
                                 werr);
      if (!walked && err.empty()) err = werr;
    }
    uint64_t qused = 0, qtotal = 0;
    bool quota_ok = false;
    if (have_meta && !access.empty()) {
      std::string qerr;
      quota_ok = drive_about_quota(access, qused, qtotal, qerr);
    }
    // Media probing via a temp download (images + audio/video).
    std::string tmp;
    bool want_image = false;
    bool want_media = false;
    std::string export_mime; // non-empty when the file needs files.export
    if (have_meta && !meta.is_dir) {
      // Decide by Drive mime + name extension, mirroring the local rules.
      std::string nm;
      {
        std::string r = drive_display_path(path);
        auto s2 = r.rfind('/');
        std::string nr = (s2 == std::string::npos) ? r : r.substr(s2 + 1);
        gchar* u2 = g_uri_unescape_string(nr.c_str(), nullptr);
        nm = u2 ? u2 : nr;
        g_free(u2);
      }
      std::string ext;
      auto dot = nm.rfind('.');
      if (dot != std::string::npos) {
        ext = nm.substr(dot);
        for (auto& c : ext)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      std::string export_suffix;
      export_mime = drive_export_target(meta.mime, export_suffix);
      // Visual types get the Image tab (identify on the download/export);
      // Docs/Sheets/etc. export fine but aren't images — skip probing.
      bool visual = meta.mime.rfind("image/", 0) == 0 ||
                    meta.mime == "application/vnd.google-apps.drawing" ||
                    meta.mime == "application/vnd.google-apps.photo" ||
                    meta.mime == "application/vnd.google-apps.pic";
      if (visual ||
          std::find(img_exts.begin(), img_exts.end(), ext) != img_exts.end())
        want_image = true;
      else if (std::find(media_exts.begin(), media_exts.end(), ext) !=
               media_exts.end())
        want_media = true;
      if ((want_image || want_media) && meta.size > 0 &&
          meta.size <= 100u * 1024u * 1024u) {
        std::string suffix = ext;
        if (!export_mime.empty()) suffix = export_suffix;
        tmp = std::string(g_get_tmp_dir()) + "/horizon-props-" +
              std::to_string(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count()) +
              suffix;
        std::vector<uint8_t> bytes;
        std::string derr;
        bool fetched =
            export_mime.empty()
                ? drive_download(access, loc.id, bytes, derr, loc.drive_id)
                : drive_export(access, loc.id, export_mime, bytes, derr,
                               loc.drive_id);
        if (fetched && !bytes.empty()) {
          FILE* f = fopen(tmp.c_str(), "wb");
          if (f) {
            size_t w = fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
            if (w != bytes.size()) {
              std::remove(tmp.c_str());
              tmp.clear();
            }
          } else {
            tmp.clear();
          }
        } else {
          tmp.clear();
        }
      }
    }
    DeferredCall::callLater(
        [ap, path, gen, meta, have_meta, tree, walked, qused, qtotal,
         quota_ok, tmp, want_image, want_media]() {
          auto& p = ap->properties;
          if (!p.open || p.multi || p.props_gen != gen || p.path != path)
            return;
          if (have_meta) {
            p.is_dir = meta.is_dir;
            p.size = meta.is_dir ? tree.bytes : meta.size;
            p.contained_files = tree.files;
            p.contained_dirs = tree.dirs;
            p.modified_sec = meta.mtime;
            p.created_sec = meta.ctime;
            p.accessed_sec = meta.viewed;
            if (const char* label = drive_kind_label(meta.mime))
              p.mime_type = label;
            else
              p.mime_type = meta.mime;
            if (!meta.owner.empty()) {
              p.owner_name = meta.owner;
              if (!meta.owner_email.empty())
                p.owner_name += " <" + meta.owner_email + ">";
            }
            if (meta.shared) p.group_name = "Shared";
          }
          if (quota_ok && qtotal > 0) {
            p.vol_total_bytes = qtotal;
            p.vol_free_bytes = qtotal > qused ? qtotal - qused : 0;
          }
          if (!tmp.empty()) {
            if (want_image) probe_image_file(tmp, p);
            if (want_media) probe_media_file(tmp, p);
            std::remove(tmp.c_str());
          }
          p.dir_size_pending = false;
          draw_props_window(*ap);
        });
  }).detach();
}

void show_properties(AppState& app, const std::string& path, const std::string& icon_name) {
  auto& p = app.properties;
  const uint64_t gen = p.props_gen + 1;
  p = AppState::PropertiesState{};
  p.props_gen = gen;
  p.open = true;
  p.path = path;
  p.icon_name = icon_name;
  if (is_drive_uri(path)) {
    show_drive_properties(app, path, icon_name, gen);
    return;
  }
  p.location = fs::path(path).parent_path().string();

  struct stat st;
  if (stat(path.c_str(), &st) != 0) return;

  p.is_dir = S_ISDIR(st.st_mode);
  if (p.is_dir) {
    // Large trees blocked the UI thread here (recursive walk + stat storm
    // on folders like Home). Open immediately and size in the background.
    p.dir_size_pending = true;
    std::thread([&app, path, gen]() {
      DirSize r;
      try {
        r = walk_dir_size(path);
      } catch (...) {
        // walk_dir_size is noexcept; this guards future edits. Never let
        // a detached probe terminate the process (SIGABRT, whole app).
      }
      DeferredCall::callLater([&app, path, gen, r]() {
        auto& p = app.properties;
        if (!p.open || p.multi || p.props_gen != gen || p.path != path) return;
        p.contained_files = r.files;
        p.contained_dirs = r.dirs;
        p.size = r.bytes;
        p.dir_size_pending = false;
        draw_props_window(app);
      });
    }).detach();
  } else {
    p.size = static_cast<uint64_t>(st.st_size);
  }

  // Volume usage for the filesystem holding this item (donut in Basic tab)
  {
    struct statvfs vfs;
    if (statvfs(path.c_str(), &vfs) == 0 && vfs.f_frsize > 0) {
      p.vol_total_bytes =
          static_cast<uint64_t>(vfs.f_blocks) * static_cast<uint64_t>(vfs.f_frsize);
      p.vol_free_bytes =
          static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
    }
  }

  // Tags (freedesktop user.xdg.tags xattr)
  p.tags_value = read_xdg_tags(path);
  // Rating + comment xattrs
  p.rating_value = read_xdg_rating(path);
  p.comment_value = read_xdg_comment(path);
  p.comment_edit = false;
  p.comment_buf.clear();

  p.modified_sec = st.st_mtime;
  p.accessed_sec = st.st_atime;
  p.created_sec = st.st_ctime;

  p.current_mode = st.st_mode;
  // Compute combo values
  auto perm_level = [](bool r, bool w, bool x) {
    if (!r) return 0;
    if (!w) return 1;
    if (!x) return 2;
    return 3;
  };
  p.perm_owner = perm_level(st.st_mode & S_IRUSR, st.st_mode & S_IWUSR, st.st_mode & S_IXUSR);
  p.perm_group = perm_level(st.st_mode & S_IRGRP, st.st_mode & S_IWGRP, st.st_mode & S_IXGRP);
  p.perm_other = perm_level(st.st_mode & S_IROTH, st.st_mode & S_IWOTH, st.st_mode & S_IXOTH);
  p.executable = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
  // Detect files that can be made executable (extension-based)
  if (!p.is_dir) {
    static const std::vector<std::string> exec_exts = {
      ".sh", ".bash", ".zsh", ".fish", ".csh", ".ksh",
      ".bin", ".elf", ".exe", ".msi", ".out", ".app", ".run",
      ".com", ".bat", ".cmd", ".ps1",
      ".appimage", ".desktop", ".deb", ".rpm", ".appdir", ".flatpak", ".snap",
      ".py", ".pl", ".rb", ".lua", ".js", ".ts", ".php",
    };
    std::string fname = fs::path(path).filename().string();
    auto dotpos = fname.rfind('.');
    if (dotpos != std::string::npos) {
      std::string ext = fname.substr(dotpos);
      for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      p.can_be_executable = std::find(exec_exts.begin(), exec_exts.end(), ext) != exec_exts.end();
    }
  }

  // Owner/group names
  struct passwd* pw = getpwuid(st.st_uid);
  p.owner_name = pw ? pw->pw_name : std::to_string(st.st_uid);
  struct group* gr = getgrgid(st.st_gid);
  p.group_name = gr ? gr->gr_name : std::to_string(st.st_gid);

  p.name = fs::path(path).filename().string();
  if (p.name.empty()) p.name = path;

  // MIME type
  if (!p.is_dir) {
    std::string cmd = "xdg-mime query filetype '" + path + "' 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (f) {
      char buf[256];
      if (fgets(buf, sizeof(buf), f)) {
        std::string mime(buf);
        while (!mime.empty() && (mime.back() == '\n' || mime.back() == '\r'))
          mime.pop_back();
        p.mime_type = mime;
      }
      pclose(f);
    }
  } else {
    p.mime_type = "inode/directory";
  }

  // MIME-based executable fallback (after MIME query)
  if (!p.can_be_executable && !p.is_dir) {
    static const std::vector<std::string> exec_mimes = {
      "application/x-executable", "application/x-elf",
      "application/x-sharedlib", "application/x-pie-executable",
      "application/vnd.microsoft.portable-executable",
      "application/x-ms-dos-executable", "application/x-msdownload",
      "application/x-appimage",
    };
    for (const auto& m : exec_mimes) {
      if (p.mime_type == m) { p.can_be_executable = true; break; }
    }
    if (!p.can_be_executable && (p.mime_type.find("x-rpm") != std::string::npos ||
        p.mime_type.find("x-flatpak") != std::string::npos ||
        p.mime_type.find("x-snap") != std::string::npos))
      p.can_be_executable = true;
  }

  // Helper: shell-escape a path for single-quote quoting

  // Image dimensions — ext list mirrors detect_file_type()'s Image set plus
  // common raster containers identify/ffprobe can read (jxl, jp2, tga, dds,
  // exr, hdr, qoi, cur, icns). MIME is the backstop so renamed/odd
  // extensions still get an Image tab when xdg-mime says image/*.
  std::string ext;
  auto dot = p.name.rfind('.');
  if (dot != std::string::npos) {
    ext = p.name.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  bool is_image = !ext.empty() && std::find(img_exts.begin(), img_exts.end(), ext) != img_exts.end();
  if (!is_image && p.mime_type.rfind("image/", 0) == 0) is_image = true;
  if (is_image) probe_image_file(path, p);
  if (!is_image && !ext.empty() &&
      std::find(media_exts.begin(), media_exts.end(), ext) !=
          media_exts.end())
    probe_media_file(path, p);

  p.scroll_px = 0;
  p.combo_open = -1;
  p.combo_hover_item = -1;
  create_props_window(app);
}

void show_properties_multi(AppState& app, const std::vector<std::string>& paths) {
  auto& p = app.properties;
  const uint64_t gen = p.props_gen + 1;
  p = AppState::PropertiesState{};
  p.props_gen = gen;
  p.open = true;
  p.multi = true;
  p.paths = paths;

  uint64_t total_size = 0;
  bool have_representative = false;
  std::vector<std::string> multi_dirs;

  // Executable-capable extensions (same list as single-item properties)
  static const std::vector<std::string> exec_exts = {
    ".sh", ".bash", ".zsh", ".fish", ".csh", ".ksh",
    ".bin", ".elf", ".exe", ".msi", ".out", ".app", ".run",
    ".com", ".bat", ".cmd", ".ps1",
    ".appimage", ".desktop", ".deb", ".rpm", ".appdir", ".flatpak", ".snap",
    ".py", ".pl", ".rb", ".lua", ".js", ".ts", ".php",
  };

  for (const auto& path : paths) {
    if (is_drive_uri(path)) {
      // Drive entries via the API (stat + walk below); representative
      // metadata stays with the first local item when there is one.
      DriveStat dst;
      std::string derr;
      if (!drive_stat_uri(path, dst, derr) || !dst.exists) continue;
      if (dst.is_dir) {
        ++p.dir_count;
        multi_dirs.push_back(path);
      } else {
        ++p.file_count;
        total_size += dst.size;
      }
      // Representative metadata stays with the first local item (see
      // below); when every item is Drive-backed the set is marked so
      // the Permissions tab stays hidden.
      continue;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      ++p.dir_count;
      multi_dirs.push_back(path);
    } else {
      ++p.file_count;
      total_size += static_cast<uint64_t>(st.st_size);
    }

    // Representative metadata from the first item: times, ownership, permissions
    if (!have_representative) {
      have_representative = true;
      p.modified_sec = st.st_mtime;
      p.accessed_sec = st.st_atime;
      p.created_sec = st.st_ctime;
      p.current_mode = st.st_mode;
      auto perm_level = [](bool r, bool w, bool x) {
        if (!r) return 0;
        if (!w) return 1;
        if (!x) return 2;
        return 3;
      };
      p.perm_owner = perm_level(st.st_mode & S_IRUSR, st.st_mode & S_IWUSR, st.st_mode & S_IXUSR);
      p.perm_group = perm_level(st.st_mode & S_IRGRP, st.st_mode & S_IWGRP, st.st_mode & S_IXGRP);
      p.perm_other = perm_level(st.st_mode & S_IROTH, st.st_mode & S_IWOTH, st.st_mode & S_IXOTH);
      p.executable = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
      struct passwd* pw = getpwuid(st.st_uid);
      p.owner_name = pw ? pw->pw_name : std::to_string(st.st_uid);
      struct group* gr = getgrgid(st.st_gid);
      p.group_name = gr ? gr->gr_name : std::to_string(st.st_gid);
    }

    if (!S_ISDIR(st.st_mode) && !p.can_be_executable) {
      std::string fname = fs::path(path).filename().string();
      auto dotpos = fname.rfind('.');
      if (dotpos != std::string::npos) {
        std::string ext = fname.substr(dotpos);
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        p.can_be_executable = std::find(exec_exts.begin(), exec_exts.end(), ext) != exec_exts.end();
      }
    }
  }

  p.size = total_size;
  if (!have_representative) {
    // No local item to represent the set: everything selected is Drive.
    p.drive_item = true;
  }
  if (!multi_dirs.empty()) {
    // Size the directory portions off the UI thread (see show_properties).
    // Drive folders walk the API; local ones walk the filesystem.
    p.dir_size_pending = true;
    std::thread([&app, paths, gen, multi_dirs, total_size]() {
      uint64_t dir_bytes = 0;
      try {
        for (const auto& d : multi_dirs) {
          if (is_drive_uri(d)) {
            uint64_t f = 0, dd = 0, b = 0;
            std::string err;
            if (drive_folder_size(d, f, dd, b, err)) dir_bytes += b;
          } else {
            dir_bytes += walk_dir_size(d).bytes;
          }
        }
      } catch (...) {
        // Partial totals below; a detached worker must never throw out.
      }
      DeferredCall::callLater([&app, paths, gen, total_size, dir_bytes]() {
        auto& p = app.properties;
        if (!p.open || !p.multi || p.props_gen != gen || p.paths != paths) return;
        p.size = total_size + dir_bytes;
        p.dir_size_pending = false;
        draw_props_window(app);
      });
    }).detach();
  }
  p.name = std::to_string(paths.size()) + (paths.size() == 1 ? " item" : " items");
  if (!paths.empty())
    p.location = is_drive_uri(paths.front())
                     ? drive_parent(paths.front())
                     : fs::path(paths.front()).parent_path().string();

  p.scroll_px = 0;
  p.combo_open = -1;
  p.combo_hover_item = -1;
  create_props_window(app);
}

} // namespace eh::file_browser
