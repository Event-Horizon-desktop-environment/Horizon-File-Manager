#include "app/file_browser/features/compress.hpp"
#include "app/file_browser/app.hpp"
#include "app/file_browser/app_types.hpp"
#include "app/file_browser/features/progress.hpp"
#include "base/thread/thread_dispatch.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
  // Remove trailing extension if present
  auto dot = base.rfind('.');
  if (dot != std::string::npos) base = base.substr(0, dot);
  base += kCompressFormats[app.compress_format].extension;
  return fs::path(app.cur_tab().current_path) / base;
}

std::string format_compress_cmd(const std::vector<std::string>& source_paths,
                                 const std::string& archive_path,
                                 int format_idx, int level) {
  std::string cmd;
  switch (format_idx) {
    case 0: { // zip
      if (tool_available("zip")) {
        cmd = "zip -r -" + std::to_string(std::min(9, std::max(0, level)));
        for (const auto& p : source_paths) cmd += " " + shell_quote(p);
        cmd += " -- " + shell_quote(archive_path);
      } else {
        cmd = "7z a -tzip -mx=" + std::to_string(std::min(9, std::max(0, level)));
        cmd += " " + shell_quote(archive_path);
        for (const auto& p : source_paths) cmd += " " + shell_quote(p);
      }
      break;
    }
    case 1: // tar.gz
      cmd = "tar -czf " + shell_quote(archive_path);
      for (const auto& p : source_paths) cmd += " " + shell_quote(p);
      break;
    case 2: // tar.bz2
      cmd = "tar -cjf " + shell_quote(archive_path);
      for (const auto& p : source_paths) cmd += " " + shell_quote(p);
      break;
    case 3: // tar.xz
      cmd = "tar -cJf " + shell_quote(archive_path);
      for (const auto& p : source_paths) cmd += " " + shell_quote(p);
      break;
    case 4: // 7z
      cmd = "7z a -mx=" + std::to_string(std::min(9, std::max(0, level / 2)));
      cmd += " " + shell_quote(archive_path);
      for (const auto& p : source_paths) cmd += " " + shell_quote(p);
      break;
    case 5: // rar
      cmd = "rar a -m" + std::to_string(std::min(5, std::max(0, level / 2)));
      cmd += " " + shell_quote(archive_path);
      for (const auto& p : source_paths) cmd += " " + shell_quote(p);
      break;
    case 6: // tar (no compression)
      cmd = "tar -cf " + shell_quote(archive_path);
      for (const auto& p : source_paths) cmd += " " + shell_quote(p);
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

void execute_compress_async(AppState& app) {
  std::string archive = archive_name_for(app);

  std::error_code ec;
  if (fs::exists(archive, ec)) fs::remove(archive, ec);

  std::string cmd = format_compress_cmd(app.compress_source_paths, archive,
                                         app.compress_format, app.compress_level);
  cmd += " 2>/dev/null";

  app.compress_dialog_open = false;
  app.operation_in_progress = true;
  app.operation_status = "Compressing...";
  draw(app);

  int ret = std::system(cmd.c_str());

  app.operation_in_progress = false;
  app.operation_status = (ret == 0) ? "Compression complete" : "Compression failed";
  app.operation_status_expires_ms = cmp_expiry_3s();
  reload_dir(app);
  draw(app);
}

// ── archive detection ────────────────────────────────────────────

static const char* kArchiveExts[] = {
  ".zip", ".tar.gz", ".tar.bz2", ".tar.xz", ".7z", ".rar", ".tar",
};

bool is_archive_extension(const std::string& path) {
  // Check double extensions first (.tar.gz, .tar.bz2, .tar.xz)
  std::string lower = path;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const auto* ext : kArchiveExts) {
    if (lower.size() >= strlen(ext) &&
        lower.compare(lower.size() - strlen(ext), strlen(ext), ext) == 0)
      return true;
  }
  return false;
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
  while ((r = archive_read_next_header(a, &ae)) == ARCHIVE_OK) {
    entries.emplace_back(archive_entry_pathname(ae), archive_entry_size(ae));
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

  while ((r = archive_read_next_header(a, &ae)) == ARCHIVE_OK) {
    if (prog->cancel.load()) break;

    std::string entry_path = archive_entry_pathname(ae);
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
    prog->current_file = std::move(fname);
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
    prog->current_file.clear();
    return;
  }

  if (shell_extract_supported(archive_path)) {
    prog->total_files.store(0);
    prog->current_file = "Extracting...";
    bool ok = do_shell_extract(archive_path, dest_dir, password, prog);
    prog->active = false;
    prog->current_file.clear();
    if (ok) return;
  }

  prog->success = false;
  prog->active = false;
  prog->current_file.clear();
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
