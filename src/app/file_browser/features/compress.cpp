#include "app/file_browser/features/compress.hpp"
#include "app/file_browser/app.hpp"
#include "app/file_browser/app_types.hpp"
#include "app/file_browser/features/progress.hpp"
#include "base/thread/thread_dispatch.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef EH_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif


namespace fs = std::filesystem;
using cmp_clock = std::chrono::steady_clock;

static std::uint64_t cmp_expiry_3s() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             (cmp_clock::now() + std::chrono::milliseconds(3000)).time_since_epoch())
      .count();
}

namespace eh::file_browser {

const CompressFormat kCompressFormats[kNumCompressFormats] = {
  {"Zip",     ".zip"},
  {"Tar.gz",  ".tar.gz"},
  {"Tar.bz2", ".tar.bz2"},
  {"Tar.xz",  ".tar.xz"},
  {"7z",      ".7z"},
  {"Rar",     ".rar"},
  {"Tar",     ".tar"},
};

static bool tool_available(const char* name);

unsigned compress_hw_threads() {
  unsigned hc = std::thread::hardware_concurrency();
  return hc == 0 ? 4 : hc;
}

std::vector<int> compress_thread_options() {
  unsigned hc = compress_hw_threads();
  std::vector<int> opts = {0};
  for (int p : {1, 2, 4, 8, 16, 32}) {
    if (p <= static_cast<int>(hc)) opts.push_back(p);
  }
  int maxv = static_cast<int>(hc);
  if (opts.back() != maxv) opts.push_back(maxv); // detected count is the max
  while (opts.size() > 7) opts.erase(opts.end() - 2); // keep Auto + max
  return opts;
}

int compress_thread_btn_w(int count) {
  if (count <= 0) return 56;
  int w = (380 + 8) / count - 8;
  if (w > 56) w = 56;
  return std::max(w, 32);
}

int compress_effective_threads(int threads) {
  if (threads > 0) return threads;
  return static_cast<int>(compress_hw_threads());
}

static std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += '\'';
  return out;
}

static std::string archive_name_for(AppState& app) {
  std::string base = app.compress_name_buf.empty() ? app.compress_source_name : app.compress_name_buf;
  // If the name already ends with a known archive extension, strip it so the
  // selected format's extension is applied cleanly (e.g. "backup.tar.gz" +
  // zip -> "backup.zip"). Otherwise leave every dot intact: a custom name like
  // "my.photos.2024" must never be truncated.
  for (int i = 0; i < kNumCompressFormats; ++i) {
    const auto& ext = kCompressFormats[i].extension;
    std::size_t elen = ext.size();
    if (base.size() > elen && base.compare(base.size() - elen, elen, ext) == 0) {
      base.resize(base.size() - elen);
      break;
    }
  }
  if (base.empty()) base = "archive";
  base += kCompressFormats[app.compress_format].extension;
  return fs::path(app.cur_tab().current_path) / base;
}

// Deepest directory that contains every source path; archive entries are then
// stored relative to it so extracting produces the folder tree the user sees,
// not the absolute path hierarchy of their machine.
static std::string common_parent_dir(const std::vector<std::string>& paths) {
  if (paths.empty()) return "/";
  std::string common = fs::path(paths[0]).parent_path().lexically_normal().string();
  for (std::size_t i = 1; i < paths.size() && !common.empty(); ++i) {
    std::string p = fs::path(paths[i]).parent_path().lexically_normal().string();
    std::size_t n = std::min(common.size(), p.size());
    std::size_t k = 0;
    while (k < n && common[k] == p[k]) ++k;
    while (k > 0 && common[k - 1] != '/') --k;
    common = common.substr(0, k);
  }
  return common.empty() ? "/" : common;
}

static std::vector<std::string> relative_basenames(const std::vector<std::string>& paths,
                                                    const std::string& parent) {
  std::vector<std::string> rels;
  std::string prefix = parent;
  if (!prefix.empty() && prefix.back() != '/') prefix += '/';
  for (const auto& p : paths) {
    std::string full = fs::absolute(p).lexically_normal().string();
    if (full.size() > prefix.size() && full.compare(0, prefix.size(), prefix) == 0)
      rels.push_back(full.substr(prefix.size()));
    else
      rels.push_back(fs::path(full).filename().string());
  }
  return rels;
}

std::string format_compress_cmd(const std::vector<std::string>& source_paths,
                                 const std::string& archive_path,
                                 int format_idx, int level, int threads) {
  if (source_paths.empty()) return std::string();
  // Archive entries are stored relative to the common parent directory so the
  // extracted layout matches the folder tree on screen (no nested machine paths).
  std::string parent = common_parent_dir(source_paths);
  auto rels = relative_basenames(source_paths, parent);
  std::string qparent = shell_quote(parent);
  std::string qarchive = shell_quote(archive_path);
  std::string qrels;
  for (const auto& r : rels) qrels += " " + shell_quote(r);

  // Threading: 0 = Auto (all cores). zip is single-threaded and plain tar
  // does no compression, so both ignore this; pigz/pbzip2 are used
  // opportunistically for gz/bz2 when installed (byte-compatible output).
  int eff = compress_effective_threads(threads);
  int lvl = std::min(9, std::max(0, level));

  std::string cmd;
  switch (format_idx) {
    case 0: { // zip (single-threaded)
      if (tool_available("zip"))
        cmd = "cd " + qparent + " && zip -r -" +
              std::to_string(lvl) +
              " " + qarchive + qrels;
      else
        cmd = "cd " + qparent + " && 7z a -bsp1 -tzip -mmt=" +
              std::to_string(eff) + " -mx=" +
              std::to_string(lvl) +
              " " + qarchive + qrels;
      break;
    }
    case 1: { // tar.gz
      if (eff > 1 && tool_available("pigz"))
        cmd = "tar -cvf " + qarchive + " -I 'pigz -p" +
              std::to_string(eff) + " -" + std::to_string(lvl) +
              "' -C " + qparent + qrels;
      else
        cmd = "tar -czvf " + qarchive + " -C " + qparent + qrels;
      break;
    }
    case 2: { // tar.bz2
      if (eff > 1 && tool_available("pbzip2"))
        cmd = "tar -cvf " + qarchive + " -I 'pbzip2 -p" +
              std::to_string(eff) + " -" +
              std::to_string(std::min(9, std::max(1, lvl))) +
              "' -C " + qparent + qrels;
      else
        cmd = "tar -cjvf " + qarchive + " -C " + qparent + qrels;
      break;
    }
    case 3: { // tar.xz (threads + level via XZ_OPT; -T0 = xz auto)
      std::string topt = (threads <= 0) ? "-T0" : ("-T" + std::to_string(eff));
      cmd = "XZ_OPT='" + topt + " -" + std::to_string(lvl) + "' tar -cJvf " +
            qarchive + " -C " + qparent + qrels;
      break;
    }
    case 4: // 7z
      cmd = "cd " + qparent + " && 7z a -bsp1 -mmt=" +
            std::to_string(eff) + " -mx=" +
            std::to_string(std::min(9, std::max(0, level / 2))) +
            " " + qarchive + qrels;
      break;
    case 5: // rar
      cmd = "cd " + qparent + " && rar a -mt" +
            std::to_string(eff) + " -m" +
            std::to_string(std::min(5, std::max(0, level / 2))) +
            " " + qarchive + qrels;
      break;
    case 6: // tar (no compression)
      cmd = "tar -cvf " + qarchive + " -C " + qparent + qrels;
      break;
  }
  return cmd;
}

static bool tool_available(const char* name) {
  std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

void check_compress_tool_availability(AppState& app) {
  // format 0 (zip), 1 (tar.gz), 2 (tar.bz2), 3 (tar.xz)
  app.compress_format_available[0] = tool_available("zip") || tool_available("7z");
  app.compress_format_available[1] = tool_available("tar") && tool_available("gzip");
  app.compress_format_available[2] = tool_available("tar") && tool_available("bzip2");
  app.compress_format_available[3] = tool_available("tar") && tool_available("xz");
  app.compress_format_available[4] = tool_available("7z");
  app.compress_format_available[5] = tool_available("rar");
  app.compress_format_available[6] = tool_available("tar");

  // Auto-switch to first available format if current one is unavailable
  if (!app.compress_format_available[app.compress_format]) {
    for (int i = 0; i < 7; ++i) {
      if (app.compress_format_available[i]) {
        app.compress_format = i;
        break;
      }
    }
  }
}

// Which archiver backend will run: decides how stdout is parsed for progress.
// Mirrors the tool choice in format_compress_cmd (zip falls back to 7z).
enum class CompressBackend { Zip, Tar, SevenZ, Rar };

static CompressBackend compress_backend(int format_idx) {
  if (format_idx == 0)
    return tool_available("zip") ? CompressBackend::Zip : CompressBackend::SevenZ;
  if (format_idx == 4) return CompressBackend::SevenZ;
  if (format_idx == 5) return CompressBackend::Rar;
  return CompressBackend::Tar;
}

// Enumerate every regular source file as (archive-relative name, size).
// Relative names use the same prefix-strip as relative_basenames() so they
// match both the argv names handed to the tool and the names the tool echoes
// back on stdout (zip "adding:" lines, tar -v member lines).
static std::vector<std::pair<std::string, uint64_t>> enumerate_compress_sources(
    const std::vector<std::string>& paths, const std::string& parent) {
  std::vector<std::pair<std::string, uint64_t>> out;
  std::string prefix = parent;
  if (!prefix.empty() && prefix.back() != '/') prefix += '/';
  auto rel_of = [&](const std::string& full) {
    if (full.size() > prefix.size() && full.compare(0, prefix.size(), prefix) == 0)
      return full.substr(prefix.size());
    return fs::path(full).filename().string();
  };
  std::error_code ec;
  for (const auto& p : paths) {
    std::string full = fs::absolute(p).lexically_normal().string();
    if (fs::is_directory(p, ec)) {
      for (auto& de : fs::recursive_directory_iterator(p, ec)) {
        if (de.is_regular_file(ec)) {
          std::string ffull = fs::absolute(de.path().string()).lexically_normal().string();
          out.emplace_back(rel_of(ffull), de.file_size(ec));
        }
      }
    } else {
      out.emplace_back(rel_of(full), fs::file_size(p, ec));
    }
  }
  return out;
}

static std::string trim_lr(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\b' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\b' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

// Extract a candidate archived name from one tool output line, or empty.
// zip:   "  adding: rel/path (deflated 12%)"  (suffix cut keeps spaced names)
// tar:   whole verbose member line ("rel/path", "dir/" skipped by lookup)
// 7z:    " 12% + rel/path"  (name runs to EOL; backspaces stripped)
// rar:   best effort — any whitespace token is looked up by the caller.
static std::string compress_line_name(const std::string& line, CompressBackend backend) {
  if (backend == CompressBackend::Zip) {
    // Fresh archives print "adding:"; pre-existing ones "updating:"
    // (we delete first, but match both to be safe).
    size_t pos = line.find("adding:");
    if (pos == std::string::npos) pos = line.find("updating:");
    if (pos == std::string::npos) return {};
    std::string rest = trim_lr(line.substr(pos + (line[pos] == 'a' ? 7 : 9)));
    for (const char* suf : {" (stored", " (deflated"}) {
      auto cut = rest.find(suf);
      if (cut != std::string::npos) { rest.resize(cut); break; }
    }
    return trim_lr(rest);
  }
  if (backend == CompressBackend::Tar) {
    return trim_lr(line);
  }
  if (backend == CompressBackend::SevenZ) {
    auto pos = line.find('+');
    if (pos == std::string::npos || pos + 1 >= line.size()) return {};
    return trim_lr(line.substr(pos + 1));
  }
  return {};
}

// Overall percent tokens ("12%") in a line. Only meaningful for 7z, whose
// percentage tracks the whole operation — zip's "(deflated N%)" is per-file
// and rar's is unreliable, so those backends never use this signal.
static double compress_line_pct(const std::string& line) {
  double best = -1.0;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '%' && i > 0) {
      size_t j = i;
      while (j > 0 && line[j - 1] >= '0' && line[j - 1] <= '9') --j;
      if (j < i && i - j <= 3) {
        double v = 0;
        for (size_t k = j; k < i; ++k) v = v * 10 + (line[k] - '0');
        if (v >= 0 && v <= 100 && v > best) best = v;
      }
    }
  }
  return best;
}

// Run the archiver with stdout piped, streaming tool output into progress:
//   % bar  = max(completed-files fraction, 7z overall %, output-growth floor)
//   speed/ETA come from exact completed input bytes (existing panel math).
// Returns the tool exit code, or -1 when cancelled (caller removes output).
static int run_compress_piped(const std::string& cmd,
                              const std::shared_ptr<OperationProgress>& prog,
                              const std::string& archive,
                              CompressBackend backend,
                              const std::unordered_map<std::string, uint64_t>& sizes,
                              int total, uint64_t total_bytes) {
  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) {
    int ret = std::system(cmd.c_str());
    return prog->cancel.load() ? -1 : ret;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    int ret = std::system(cmd.c_str());
    return prog->cancel.load() ? -1 : ret;
  }
  if (pid == 0) {
    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
    close(pipefd[1]);
    setpgid(0, 0);
    execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  close(pipefd[1]);
  setpgid(pid, pid); // best effort; harmless race if the child exec'd first
  int flags = fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  std::unordered_set<std::string> done;
  done.reserve(sizes.size() * 2 + 1);
  int done_count = 0;
  double overall_pct = -1.0;
  std::string cur_file = prog->get_current_file();
  std::string buf;
  char chunk[65536];
  bool eof = false;
  int status = 0;
  bool child_done = false;

  auto publish = [&]() {
    // Single input-equivalent fraction drives bar, bytes, speed and ETA:
    // exact file completions, 7z overall %, live output size. Output size is
    // only an input stand-in (exact for incompressible data like video),
    // but it moves from the first bytes written, unlike whole-file steps.
    // 7z buffers its output (headers first, bulk at the end), so its % —
    // separated by backspaces, not newlines — is the live signal there.
    double in_frac = total > 0 ? static_cast<double>(done_count) / total : 0.0;
    if (backend == CompressBackend::SevenZ && overall_pct >= 0)
      in_frac = std::max(in_frac, overall_pct / 100.0);
    uint64_t out_size = 0;
    if (total_bytes > 0) {
      struct stat st{};
      if (::stat(archive.c_str(), &st) == 0 && st.st_size > 0)
        out_size = static_cast<uint64_t>(st.st_size);
      if (out_size > 0)
        in_frac = std::max(in_frac, static_cast<double>(out_size) / total_bytes);
    }
    if (in_frac < 0) in_frac = 0;
    if (in_frac > 1) in_frac = 1;
    prog->progress.store(in_frac);
    prog->copied_files.store(done_count);
    prog->done_bytes.store(static_cast<uint64_t>(in_frac * total_bytes));
  };

  auto parse_segment = [&](const std::string& seg) {
    if (backend == CompressBackend::SevenZ) {
      double v = compress_line_pct(seg);
      if (v >= 0 && v > overall_pct) overall_pct = v;
    }
    std::string name = compress_line_name(seg, backend);
    auto emit_name = [&](const std::string& n) {
      if (n.empty() || done.count(n)) return;
      auto it = sizes.find(n);
      if (it == sizes.end()) return;
      done.insert(n);
      ++done_count;
      std::string base = fs::path(n).filename().string();
      if (base.size() > 40) base = base.substr(0, 37) + "...";
      if (base != cur_file) {
        cur_file = base;
        prog->set_current_file(cur_file);
      }
    };
    emit_name(name);
    if (backend == CompressBackend::Rar) {
      // rar has no reliable % signal: match any whitespace token instead.
      size_t a = 0;
      while (a < seg.size()) {
        while (a < seg.size() && (seg[a] == ' ' || seg[a] == '\t')) ++a;
        size_t b = a;
        while (b < seg.size() && seg[b] != ' ' && seg[b] != '\t') ++b;
        if (b > a) emit_name(seg.substr(a, b - a));
        a = b;
      }
    }
  };

  while (!child_done) {
    pollfd pfd{pipefd[0], POLLIN, 0};
    int pr = poll(&pfd, 1, 50);
    if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      ssize_t n;
      while ((n = ::read(pipefd[0], chunk, sizeof(chunk))) > 0) {
        buf.append(chunk, static_cast<size_t>(n));
        size_t start = 0;
        for (size_t i = 0; i < buf.size(); ++i) {
          // 7z separates live progress with backspaces, not newlines.
          if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\b') {
            if (i > start) parse_segment(buf.substr(start, i - start));
            start = i + 1;
          }
        }
        buf.erase(0, start);
      }
      if (n == 0) eof = true;
    }
    publish();
    if (prog->cancel.load()) {
      kill(-pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      close(pipefd[0]);
      return -1;
    }
    if (eof) {
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      child_done = true;
    } else {
      pid_t w = waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        // Child exited but pipe may still hold output: drain it, then reap.
        ssize_t n;
        while ((n = ::read(pipefd[0], chunk, sizeof(chunk))) > 0) {
          buf.append(chunk, static_cast<size_t>(n));
          size_t start = 0;
          for (size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\b') {
              if (i > start) parse_segment(buf.substr(start, i - start));
              start = i + 1;
            }
          }
          buf.erase(0, start);
        }
        if (!buf.empty()) parse_segment(buf);
        publish();
        child_done = true;
      } else if (w < 0 && errno != EINTR && errno != ECHILD) {
        break;
      }
    }
  }
  close(pipefd[0]);
  if (prog->cancel.load()) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 1;
}

void execute_compress_async(AppState& app) {
  std::string archive = archive_name_for(app);

  std::error_code ec;
  if (fs::exists(archive, ec)) fs::remove(archive, ec);

  std::string cmd = format_compress_cmd(app.compress_source_paths, archive,
                                         app.compress_format, app.compress_level,
                                         app.compress_threads);
  if (cmd.empty()) {
    app.operation_status = "Compression failed";
    app.operation_status_expires_ms = cmp_expiry_3s();
    draw(app);
    return;
  }
  cmd += " 2>/dev/null";

  std::vector<std::string> sources = app.compress_source_paths;

  auto prog = std::make_shared<OperationProgress>();
  prog->type = OperationType::Compress;
  prog->active.store(true);
  prog->cancel.store(false);
  prog->progress.store(0.0);
  prog->start_time = std::chrono::steady_clock::now();
  prog->set_current_file(fs::path(archive).filename().string());

  app.compress_dialog_open = false;
  app.op_progress = prog;
  app.ops_panel_open = true;
  app.operation_in_progress = true;
  app.operation_status = "Compressing...";
  draw(app);

  std::thread([&app, prog, cmd, archive, sources, format_idx = app.compress_format]() {
    // Phase 1: enumerate sources (names must match tool stdout echo).
    std::string parent = common_parent_dir(sources);
    auto files = enumerate_compress_sources(sources, parent);
    std::unordered_map<std::string, uint64_t> sizes;
    sizes.reserve(files.size() * 2 + 1);
    uint64_t total_bytes = 0;
    for (auto& [rel, sz] : files) {
      if (sizes.emplace(rel, sz).second) total_bytes += sz;
    }
    int total = static_cast<int>(sizes.size());
    prog->total_files.store(total);
    prog->total_bytes.store(total_bytes);

    int ret = -2;
    if (!prog->cancel.load())
      ret = run_compress_piped(cmd, prog, archive, compress_backend(format_idx),
                               sizes, total, total_bytes);

    bool cancelled = (ret == -1) || prog->cancel.load();
    if (cancelled) {
      std::error_code ec2;
      fs::remove(archive, ec2);
      prog->success.store(false);
    } else {
      prog->success.store(ret == 0);
      if (ret == 0) {
        // Snap to full: tool-specific echoes can miss entries (locales,
        // odd names), but a zero exit means the archive is complete.
        prog->progress.store(1.0);
        prog->copied_files.store(total);
        prog->done_bytes.store(total_bytes);
      }
    }

    bool ok = prog->success.load();
    prog->active.store(false);
    prog->clear_current_file();

    DeferredCall::callLater([&app, cancelled, ok]() {
      app.operation_in_progress = false;
      if (cancelled)
        app.operation_status = "Compression cancelled";
      else if (ok)
        app.operation_status = "Compression complete";
      else
        app.operation_status = "Compression failed";
      app.operation_status_expires_ms = cmp_expiry_3s();
      reload_dir(app);
      draw(app);
    });
  }).detach();
}

// ── archive detection ────────────────────────────────────────────

static const char* kArchiveExts[] = {
  ".zip", ".tar.gz", ".tar.bz2", ".tar.xz", ".tgz", ".7z", ".rar", ".tar",
};

bool is_archive_extension(const std::string& path) {
  // Check double extensions first (.tar.gz, .tar.bz2, .tar.xz)
  for (const auto* ext : kArchiveExts) {
    size_t n = path.size(), m = strlen(ext);
    if (n < m) continue;
    bool hit = true;
    for (size_t i = 0; i < m; ++i) {
      char a = path[n - m + i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (a != ext[i]) { hit = false; break; }
    }
    if (hit) return true;
  }
  return false;
}

// Disk images that UDisks2 can loop-mount (iso9660/udf filesystems).
// Proprietary layouts (.bin/.cue, .nrg, .mdf) are excluded — offer Extract.
bool is_iso_image(const std::string& path) {
  auto ends_ci = [](const std::string& p, const char* ext) {
    size_t n = p.size(), m = strlen(ext);
    if (n < m) return false;
    for (size_t i = 0; i < m; ++i) {
      char a = p[n - m + i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (a != ext[i]) return false;
    }
    return true;
  };
  return ends_ci(path, ".iso") || ends_ci(path, ".img") || ends_ci(path, ".udf");
}

std::string default_extract_dir(const std::string& archive_path) {
  fs::path p(archive_path);
  std::string stem = p.stem().string();
  // Handle .tar.com extension: foo.tar.gz → stem is "foo.tar", we want "foo"
  std::string ext = p.extension().string();
  if (ext == ".gz" || ext == ".bz2" || ext == ".xz") {
    stem = fs::path(stem).stem().string();
  }
  return (p.parent_path() / stem).string();
}

static std::string format_extract_cmd_internal(const std::string& archive_path,
                                                 const std::string& dest_dir) {
  std::string lower = archive_path;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  std::string qsrc = shell_quote(archive_path);
  std::string qdst = shell_quote(dest_dir);

  if (lower.ends_with(".zip"))
    return "unzip -o " + qsrc + " -d " + qdst;
  if (lower.ends_with(".tar.gz") || lower.ends_with(".tgz"))
    return "tar -xzf " + qsrc + " -C " + qdst;
  if (lower.ends_with(".tar.bz2"))
    return "tar -xjf " + qsrc + " -C " + qdst;
  if (lower.ends_with(".tar.xz"))
    return "tar -xJf " + qsrc + " -C " + qdst;
  if (lower.ends_with(".7z"))
    return "7z x " + qsrc + " -o" + qdst;
  if (lower.ends_with(".rar"))
    return "unrar x -o+ " + qsrc + " " + qdst;
  if (lower.ends_with(".tar"))
    return "tar -xf " + qsrc + " -C " + qdst;

  return {};
}

std::string format_extract_cmd(const std::string& archive_path,
                                const std::string& dest_dir) {
  return format_extract_cmd_internal(archive_path, dest_dir);
}

#ifdef EH_HAVE_LIBARCHIVE

static std::string sanitize_archive_path(const std::string& raw) {
  std::string p = raw;
  while (!p.empty() && p[0] == '/') p.erase(p.begin());
  std::istringstream ss(p);
  std::string seg;
  std::vector<std::string> clean;
  while (std::getline(ss, seg, '/')) {
    if (seg == "..") {
      if (!clean.empty()) clean.pop_back();
    } else if (!seg.empty() && seg != ".") {
      clean.push_back(seg);
    }
  }
  std::string result;
  for (size_t i = 0; i < clean.size(); ++i) {
    if (i > 0) result += '/';
    result += clean[i];
  }
  return result;
}

bool archive_is_encrypted(const std::string& archive_path) {
  struct archive* a = archive_read_new();
  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);

  if (archive_read_open_filename(a, archive_path.c_str(), 10240) != ARCHIVE_OK) {
    archive_read_free(a);
    return false;
  }

  int has_enc = archive_read_has_encrypted_entries(a);

  bool encrypted = false;
  struct archive_entry* ae = nullptr;
  int r = ARCHIVE_OK;
  while ((r = archive_read_next_header(a, &ae)) == ARCHIVE_OK) {
    if (archive_entry_is_encrypted(ae)) {
      encrypted = true;
      break;
    }
  }

  if (!encrypted && r != ARCHIVE_OK && r != ARCHIVE_EOF) {
    const char* err = archive_error_string(a);
    if (err) {
      std::string msg = err;
      for (auto& c : msg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (msg.find("encrypt") != std::string::npos ||
          msg.find("password") != std::string::npos) {
        encrypted = true;
      }
    }
  }

  if (!encrypted && has_enc > 0) encrypted = true;

  archive_read_close(a);
  archive_read_free(a);
  return encrypted;
}

static bool shell_extract_supported(const std::string& archive_path) {
  std::string lower = archive_path;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower.ends_with(".rar")) return tool_available("unrar");
  if (lower.ends_with(".zip")) return tool_available("unzip") || tool_available("7z");
  if (lower.ends_with(".7z"))  return tool_available("7z");
  return false;
}

static std::string format_extract_cmd_with_password(const std::string& archive_path,
                                                     const std::string& dest_dir,
                                                     const std::string& password) {
  std::string lower = archive_path;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  std::string qsrc = shell_quote(archive_path);
  std::string qdst = shell_quote(dest_dir);
  std::string qpw  = shell_quote(password);

  if (lower.ends_with(".rar")) {
    if (tool_available("unrar"))
      return "unrar x -o+ -p" + qpw + " " + qsrc + " " + qdst;
  }
  if (lower.ends_with(".zip")) {
    if (tool_available("7z"))
      return "7z x -p" + qpw + " -y " + qsrc + " -o" + qdst;
  }
  if (lower.ends_with(".7z")) {
    if (tool_available("7z"))
      return "7z x -p" + qpw + " -y " + qsrc + " -o" + qdst;
  }
  return {};
}

static bool archive_entries_contained(const std::string& archive_path) {
  struct archive* a = archive_read_new();
  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);
  if (archive_read_open_filename(a, archive_path.c_str(), 10240) != ARCHIVE_OK) {
    archive_read_free(a);
    return false;  // can't prove safety — refuse shell fallback
  }
  bool safe = true;
  struct archive_entry* ae = nullptr;
  int r;
  while ((r = archive_read_next_header(a, &ae)) == ARCHIVE_OK) {
    const char* pn = archive_entry_pathname(ae);
    std::string p = pn ? pn : "";
    if (p.empty() || p[0] == '/') { safe = false; break; }
    size_t pos = 0;
    while (pos <= p.size()) {
      size_t slash = p.find('/', pos);
      std::string seg = (slash == std::string::npos) ? p.substr(pos)
                                                     : p.substr(pos, slash - pos);
      if (seg == "..") { safe = false; break; }
      if (slash == std::string::npos) break;
      pos = slash + 1;
    }
    if (!safe) break;
  }
  archive_read_close(a);
  archive_read_free(a);
  return safe;
}

static bool do_shell_extract(const std::string& archive_path,
                              const std::string& dest_dir,
                              const std::string& password,
                              std::shared_ptr<OperationProgress> prog) {
  std::string cmd;
  if (!password.empty())
    cmd = format_extract_cmd_with_password(archive_path, dest_dir, password);
  if (cmd.empty())
    cmd = format_extract_cmd_internal(archive_path, dest_dir);
  if (cmd.empty()) return false;

  // ZipSlip gate: shell tools (unzip/tar/7z/unrar) do not sanitize entry
  // names, so only fall back to them when a libarchive listing proves every
  // entry stays inside dest_dir (no absolute paths, no ".." segments).
  // Otherwise refuse rather than extract outside the destination.
  if (!archive_entries_contained(archive_path)) return false;

  cmd += " 2>/dev/null";
  int ret = std::system(cmd.c_str());
  return ret == 0;
}

static bool do_libarchive_extract_inner(const std::string& archive_path,
                                         const std::string& dest_dir,
                                         std::shared_ptr<OperationProgress> prog,
                                         const std::string& password = {}) {
  struct archive* a = archive_read_new();
  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);

  if (!password.empty()) {
    archive_read_add_passphrase(a, password.c_str());
  }

  if (archive_read_open_filename(a, archive_path.c_str(), 10240) != ARCHIVE_OK) {
    archive_read_free(a);
    return false;
  }

  struct archive_entry* ae = nullptr;
  int r;

  std::vector<std::pair<std::string, int64_t>> entries;
  while (true) {
    r = archive_read_next_header(a, &ae);
    if (r == ARCHIVE_EOF) break;
    if (r == ARCHIVE_RETRY) continue;
    if (r < ARCHIVE_WARN) break;
    const char* pn = archive_entry_pathname(ae);
    if (!pn) {
      archive_read_data_skip(a);
      continue;
    }
    entries.emplace_back(pn, archive_entry_size(ae));
  }

  if (entries.empty() && r != ARCHIVE_EOF) {
    archive_read_close(a);
    archive_read_free(a);
    return false;
  }

  archive_read_close(a);
  archive_read_free(a);

  int total = static_cast<int>(entries.size());
  uint64_t total_bytes = 0;
  for (auto& [name, sz] : entries)
    if (sz > 0) total_bytes += static_cast<uint64_t>(sz);
  prog->total_files.store(total);
  prog->total_bytes.store(total_bytes);
  prog->start_time = std::chrono::steady_clock::now();

  a = archive_read_new();
  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);

  if (!password.empty()) {
    archive_read_add_passphrase(a, password.c_str());
  }

  if (archive_read_open_filename(a, archive_path.c_str(), 10240) != ARCHIVE_OK) {
    archive_read_free(a);
    return false;
  }

  struct archive* disk = archive_write_disk_new();
  archive_write_disk_set_options(disk,
    ARCHIVE_EXTRACT_TIME |
    ARCHIVE_EXTRACT_PERM |
    ARCHIVE_EXTRACT_ACL |
    ARCHIVE_EXTRACT_FFLAGS |
    ARCHIVE_EXTRACT_SECURE_SYMLINKS |
    ARCHIVE_EXTRACT_SECURE_NODOTDOT |
    ARCHIVE_EXTRACT_UNLINK);
  archive_write_disk_set_standard_lookup(disk);

  int processed = 0;
  ae = nullptr;

  while (true) {
    r = archive_read_next_header(a, &ae);
    if (r == ARCHIVE_EOF) break;
    if (r == ARCHIVE_RETRY) continue;
    if (r < ARCHIVE_WARN) break;
    if (prog->cancel.load()) break;

    const char* pn = archive_entry_pathname(ae);
    std::string entry_path = pn ? pn : "";
    if (entry_path.empty()) { archive_read_data_skip(a); continue; }
    std::string clean = sanitize_archive_path(entry_path);
    if (clean.empty()) { archive_read_data_skip(a); continue; }

    std::string full = dest_dir + "/" + clean;
    archive_entry_set_pathname(ae, full.c_str());

    const char* hardlink = archive_entry_hardlink(ae);
    if (hardlink) {
      std::string hl_clean = sanitize_archive_path(hardlink);
      std::string hl_full = dest_dir + "/" + hl_clean;
      archive_entry_set_hardlink(ae, hl_full.c_str());
    }

    r = archive_write_header(disk, ae);
    if (r != ARCHIVE_OK && r < ARCHIVE_WARN) {
      archive_read_data_skip(a);
    } else {
      if (archive_entry_size(ae) > 0 && !hardlink) {
        char buf[65536];
        ssize_t len;
        while ((len = archive_read_data(a, buf, sizeof(buf))) > 0) {
          archive_write_data(disk, buf, len);
        }
      }
      archive_write_finish_entry(disk);
    }

    ++processed;
    prog->copied_files.store(processed);
    prog->progress.store(total > 0 ? static_cast<double>(processed) / total : 0.0);
    int64_t entry_sz = archive_entry_size_is_set(ae) ? archive_entry_size(ae) : 0;
    if (entry_sz > 0)
      prog->done_bytes.fetch_add(static_cast<uint64_t>(entry_sz));

    std::string fname = fs::path(entry_path).filename().string();
    if (fname.size() > 40) fname = fname.substr(0, 37) + "...";
    prog->set_current_file(std::move(fname));
  }

  archive_write_close(disk);
  archive_write_free(disk);
  archive_read_close(a);
  archive_read_free(a);

  return true;
}

static void do_libarchive_extract(const std::string& archive_path,
                                   const std::string& dest_dir,
                                   std::shared_ptr<OperationProgress> prog,
                                   const std::string& password = {}) {
  if (do_libarchive_extract_inner(archive_path, dest_dir, prog, password)) {
    prog->active = false;
    prog->clear_current_file();
    return;
  }

  if (shell_extract_supported(archive_path)) {
    prog->total_files.store(0);
    prog->set_current_file("Extracting...");
    bool ok = do_shell_extract(archive_path, dest_dir, password, prog);
    prog->active = false;
    prog->clear_current_file();
    if (ok) return;
  }

  prog->success = false;
  prog->active = false;
  prog->clear_current_file();
}

static void start_extract_thread(AppState& app, const std::string& archive_path,
                                  const std::string& dest_dir,
                                  const std::string& password) {
  std::error_code ec;
  fs::create_directories(dest_dir, ec);

  auto prog = std::make_shared<OperationProgress>();
  prog->type = OperationType::Extract;
  prog->active.store(true);

  app.op_progress = prog;
  app.ops_panel_open = true;
  draw(app);

  std::string arc_path = archive_path;
  std::string dst_dir = dest_dir;
  std::string pw = password;

  std::thread([&app, prog, arc_path, dst_dir, pw]() {
    do_libarchive_extract(arc_path, dst_dir, prog, pw);

    bool cancelled = prog->cancel.load();
    bool success = prog->success.load();
    DeferredCall::callLater([&app, cancelled, success]() {
      if (cancelled)
        app.operation_status = "Extraction cancelled";
      else if (success)
        app.operation_status = "Extraction complete";
      else
        app.operation_status = "Extraction failed";
      app.operation_status_expires_ms = cmp_expiry_3s();
      reload_dir(app);
      draw(app);
    });
  }).detach();
}

void show_password_dialog(AppState& app, const std::string& archive_path,
                           const std::string& dest_dir) {
  app.password_dialog_open = true;
  app.password_buf.clear();
  app.password_cursor_pos = 0;
  app.password_archive_path = archive_path;
  app.password_dest_dir = dest_dir;
  draw(app);
}

void execute_extract_with_password(AppState& app, const std::string& archive_path,
                                   const std::string& dest_dir,
                                   const std::string& password) {
  start_extract_thread(app, archive_path, dest_dir, password);
}

void execute_extract_async(AppState& app, const std::string& archive_path,
                            const std::string& dest_dir) {
  if (archive_is_encrypted(archive_path)) {
    show_password_dialog(app, archive_path, dest_dir);
    return;
  }
  start_extract_thread(app, archive_path, dest_dir, {});
}

#else

bool archive_is_encrypted(const std::string&) {
  return false;
}

void show_password_dialog(AppState& app, const std::string&,
                           const std::string&) {
  app.operation_status = "libarchive not available";
  app.operation_status_expires_ms = cmp_expiry_3s();
  draw(app);
}

void execute_extract_with_password(AppState& app, const std::string& archive_path,
                                   const std::string& dest_dir,
                                   const std::string&) {
  std::error_code ec;
  fs::create_directories(dest_dir, ec);

  std::string cmd = format_extract_cmd_internal(archive_path, dest_dir);
  if (cmd.empty()) {
    app.operation_status = "Unsupported archive format";
    app.operation_status_expires_ms = cmp_expiry_3s();
    draw(app);
    return;
  }
  cmd += " 2>/dev/null";

  auto prog = std::make_shared<OperationProgress>();
  prog->type = OperationType::Extract;
  prog->active.store(true);
  app.op_progress = prog;
  app.ops_panel_open = true;
  draw(app);

  std::string shell_cmd = cmd;

  std::thread([&app, prog, shell_cmd]() {
    int ret = std::system(shell_cmd.c_str());

    prog->active = false;
    DeferredCall::callLater([&app, ret]() {
      app.operation_status = (ret == 0) ? "Extraction complete" : "Extraction failed";
      app.operation_status_expires_ms = cmp_expiry_3s();
      reload_dir(app);
      draw(app);
    });
  }).detach();
}

void execute_extract_async(AppState& app, const std::string& archive_path,
                            const std::string& dest_dir) {
  std::error_code ec;
  fs::create_directories(dest_dir, ec);

  std::string cmd = format_extract_cmd_internal(archive_path, dest_dir);
  if (cmd.empty()) {
    app.operation_status = "Unsupported archive format";
    app.operation_status_expires_ms = cmp_expiry_3s();
    draw(app);
    return;
  }
  cmd += " 2>/dev/null";

  auto prog = std::make_shared<OperationProgress>();
  prog->type = OperationType::Extract;
  prog->active.store(true);
  app.op_progress = prog;
  app.ops_panel_open = true;
  draw(app);

  std::string shell_cmd = cmd;

  std::thread([&app, prog, shell_cmd]() {
    int ret = std::system(shell_cmd.c_str());

    prog->active = false;
    DeferredCall::callLater([&app, ret]() {
      app.operation_status = (ret == 0) ? "Extraction complete" : "Extraction failed";
      app.operation_status_expires_ms = cmp_expiry_3s();
      reload_dir(app);
      draw(app);
    });
  }).detach();
}

#endif

} // namespace eh::file_browser
