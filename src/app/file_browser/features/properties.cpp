// properties.cpp — Exported from features/menu.cpp as part of the Step 5 file split.

#include "../app.hpp"
#include "app/file_browser/features/compare.hpp"
#include "app/file_browser/features/compress.hpp"
#include "app/file_browser/features/dirprops.hpp"
#include "app/file_browser/features/progress.hpp"
#include "app/file_browser/features/selection.hpp"
#include "app/file_browser/features/tab_history.hpp"
#include "app/file_browser/features/tags.hpp"
#include "app/file_browser/features/view_zoom.hpp"

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
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

void show_properties(AppState& app, const std::string& path, const std::string& icon_name) {
  auto& p = app.properties;
  p = AppState::PropertiesState{};
  p.open = true;
  p.path = path;
  p.icon_name = icon_name;
  p.location = fs::path(path).parent_path().string();

  struct stat st;
  if (stat(path.c_str(), &st) != 0) return;

  p.is_dir = S_ISDIR(st.st_mode);
  if (p.is_dir) {
    uint64_t total = 0;
    p.contained_files = 0;
    p.contained_dirs = 0;
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
      if (ec) break;
      std::error_code ec2;
      if (entry.is_regular_file(ec2) && !ec2) {
        ++p.contained_files;
        struct stat fst;
        if (stat(entry.path().c_str(), &fst) == 0)
          total += static_cast<uint64_t>(fst.st_size);
      } else if (entry.is_directory(ec2) && !ec2) {
        ++p.contained_dirs;
      }
    }
    p.size = total;
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
  auto sq = [](const std::string& s) -> std::string {
    std::string r = "'";
    for (char c : s) {
      if (c == '\'') r += "'\\''";
      else r += c;
    }
    r += '\'';
    return r;
  };

  // Image dimensions
  static const std::vector<std::string> img_exts = {
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".tiff", ".tif"
  };
  std::string ext;
  auto dot = p.name.rfind('.');
  if (dot != std::string::npos) {
    ext = p.name.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  bool is_image = !ext.empty() && std::find(img_exts.begin(), img_exts.end(), ext) != img_exts.end();
  if (is_image) {
    // First pass: dimensions via identify
    std::string icmd = "identify -format '%w %h|%[colorspace]|%[bit-depth]|%A|%C|%x|%y' " + sq(path) + " 2>/dev/null";
    FILE* f = popen(icmd.c_str(), "r");
    if (f) {
      char buf[256];
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
        p.image_has_alpha = !alpha.empty() && alpha != "None";
        p.image_compression = next_field();
        std::string res_x = next_field();
        std::string res_y = next_field();
        if (!res_x.empty() && !res_y.empty()) {
          try {
            double rx = std::stod(res_x);
            double ry = std::stod(res_y);
            if (rx > 0 && ry > 0) {
              char rbuf[32];
              snprintf(rbuf, sizeof(rbuf), "%.0f \u00d7 %.0f", rx, ry);
              p.image_resolution = rbuf;
              p.image_res_unit = (rx > 100) ? "DPI" : "DPCM";
            }
          } catch (...) {}
        }
      }
      pclose(f);
    }
    // Fallback: try ffprobe for image dimensions
    if (p.image_w == 0 || p.image_h == 0) {
      std::string fcmd = "ffprobe -v quiet -print_format json -show_streams " + sq(path) + " 2>/dev/null";
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

  // Audio/Video metadata via ffprobe
  if (!is_image && !ext.empty()) {
    static const std::vector<std::string> media_exts = {
      ".mp3", ".flac", ".ogg", ".wav", ".aac", ".m4a", ".wma", ".opus", ".ac3", ".dsf", ".aiff",
      ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".webm", ".flv", ".m4v", ".ogv", ".3gp", ".mts", ".m2ts", ".ts"
    };
    if (std::find(media_exts.begin(), media_exts.end(), ext) != media_exts.end()) {
      std::string fcmd = "ffprobe -v quiet -print_format json -show_format -show_streams " + sq(path) + " 2>/dev/null";
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
  }

  p.scroll_px = 0;
  p.combo_open = -1;
  p.combo_hover_item = -1;
  create_props_window(app);
}

void show_properties_multi(AppState& app, const std::vector<std::string>& paths) {
  auto& p = app.properties;
  p = AppState::PropertiesState{};
  p.open = true;
  p.multi = true;
  p.paths = paths;

  uint64_t total_size = 0;
  bool have_representative = false;

  // Executable-capable extensions (same list as single-item properties)
  static const std::vector<std::string> exec_exts = {
    ".sh", ".bash", ".zsh", ".fish", ".csh", ".ksh",
    ".bin", ".elf", ".exe", ".msi", ".out", ".app", ".run",
    ".com", ".bat", ".cmd", ".ps1",
    ".appimage", ".desktop", ".deb", ".rpm", ".appdir", ".flatpak", ".snap",
    ".py", ".pl", ".rb", ".lua", ".js", ".ts", ".php",
  };

  for (const auto& path : paths) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;

    if (S_ISDIR(st.st_mode)) {
      ++p.dir_count;
      uint64_t total = 0;
      std::error_code ec;
      for (auto& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
          struct stat fst;
          if (stat(entry.path().c_str(), &fst) == 0)
            total += static_cast<uint64_t>(fst.st_size);
        }
      }
      total_size += total;
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
  p.name = std::to_string(paths.size()) + (paths.size() == 1 ? " item" : " items");
  if (!paths.empty())
    p.location = fs::path(paths.front()).parent_path().string();

  p.scroll_px = 0;
  p.combo_open = -1;
  p.combo_hover_item = -1;
  create_props_window(app);
}

} // namespace eh::file_browser
