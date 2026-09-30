// remote.cpp — GVfs-backed remote browsing (SFTP in slice 1).
//
// Listings reuse the local scan slot so paint/filter/counts/history work
// unchanged: this TU joins any in-flight scan, bumps the generation, runs
// GIO on a worker thread with a private GMainContext, and publishes into
// scan_result for the normal apply path.

#include "app/file_browser/features/remote/remote.hpp"
#include "app/file_browser/features/filetype/filetype.hpp"

#include "../../app.hpp"
#include "../../trace.hpp"

#include "base/thread/thread_dispatch.hpp"
#include "base/thread/thread_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace eh::file_browser {

namespace {

// Attributes fetched per child (one round trip total, no per-file stats).
constexpr const char* kChildAttrs =
    "standard::name,standard::display-name,standard::type,"
    "standard::is-symlink,standard::symlink-target,standard::size,"
    "time::modified,time::modified-usec,standard::content-type,"
    "standard::fast-content-type,access::can-read,access::can-write";

std::string scheme_of(const std::string& uri) {
  auto pos = uri.find("://");
  if (pos == std::string::npos) return {};
  return uri.substr(0, pos);
}

} // namespace

bool is_remote_uri(const std::string& path) {
  // Remote listings go through the GIO worker. Unknown schemes stay local
  // (and fail safe) so typos can never route real paths into the worker.
  const std::string scheme = scheme_of(path);
  return scheme == "sftp" || scheme == "dav" || scheme == "davs" ||
         scheme == "google-drive";
}

std::string remote_normalize(const std::string& uri) {
  auto pos = uri.find("://");
  if (pos == std::string::npos) return uri;
  std::string head = uri.substr(0, pos + 3);
  std::string rest = uri.substr(pos + 3);
  while (rest.size() > 1 && rest.back() == '/') rest.pop_back();
  // Bare "host" is the scheme root: keep exactly one trailing slash.
  if (rest.empty() || rest.find('/') == std::string::npos) rest += '/';
  return head + rest;
}

std::string remote_parent(const std::string& uri) {
  std::string n = remote_normalize(uri);
  auto pos = n.find("://");
  if (pos == std::string::npos) return {};
  std::string rest = n.substr(pos + 3);
  if (!rest.empty() && rest.back() == '/') rest.pop_back();
  auto slash = rest.rfind('/');
  if (slash == std::string::npos) return {}; // already at root
  std::string up = rest.substr(0, slash);
  if (up.find('/') == std::string::npos) up += '/'; // root keeps its slash
  return n.substr(0, pos + 3) + up;
}

std::string build_sftp_uri(const std::string& host, const std::string& user,
                           int port, const std::string& path) {
  std::string uri = "sftp://";
  if (!user.empty()) {
    uri += user;
    uri += '@';
  }
  uri += host;
  if (port > 0 && port != 22) {
    uri += ':';
    uri += std::to_string(port);
  }
  if (path.empty() || path[0] != '/') uri += '/';
  uri += path;
  return remote_normalize(uri);
}

bool remote_scheme_available(const std::string& scheme) {
  GVfs* vfs = g_vfs_get_default();
  if (!vfs) return false;
  const char* const* schemes = g_vfs_get_supported_uri_schemes(vfs);
  if (!schemes) return false;
  for (int i = 0; schemes[i]; ++i)
    if (scheme == schemes[i]) return true;
  return false;
}

FileEntry remote_entry_from_info(GFileInfo* info, const std::string& child_uri) {
  FileEntry e;
  const char* name = info ? g_file_info_get_name(info) : nullptr;
  if (!name || !*name) {
    const char* disp = info ? g_file_info_get_display_name(info) : nullptr;
    name = (disp && *disp) ? disp : "???";
  }
  e.name = name;
  e.path = child_uri;
  GFileType t = info ? g_file_info_get_file_type(info) : G_FILE_TYPE_UNKNOWN;
  e.is_dir = (t == G_FILE_TYPE_DIRECTORY);
  e.is_symlink =
      info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) &&
      g_file_info_get_is_symlink(info);
  if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    e.size = e.is_dir ? 0 : g_file_info_get_size(info);
  if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
    GDateTime* dt = g_file_info_get_modification_date_time(info);
    if (dt) {
      e.modified_sec = g_date_time_to_unix(dt);
      g_date_time_unref(dt);
    }
  }
  const char* ct = info ? g_file_info_get_content_type(info) : nullptr;
  if (!ct && info) ct = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
  e.mime_type = ct ? ct : (e.is_dir ? "inode/directory" : "application/octet-stream");
  e.is_hidden = !e.name.empty() && e.name[0] == '.';
  if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
    e.readable = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
  if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
    e.writable = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
  if (e.is_symlink && info) {
    const char* tgt = g_file_info_get_symlink_target(info);
    if (tgt) e.link_target = tgt;
  }
  {
    auto dot = e.name.rfind('.');
    if (dot != std::string::npos && dot + 1 < e.name.size()) {
      e.extension = e.name.substr(dot + 1);
      for (auto& c : e.extension)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  e.type = detect_file_type_for_path(e.name, e.is_dir, "");
  return e;
}

void remote_cancel_inflight(AppState& app) {
  std::lock_guard<std::mutex> lk(app.scan_mtx);
  if (app.remote_cancellable) {
    g_cancellable_cancel(app.remote_cancellable);
  }
}


namespace {

GFile* vfs_gfile(const std::string& p) {
  return is_remote_uri(p) ? g_file_new_for_uri(p.c_str())
                          : g_file_new_for_path(p.c_str());
}

} // namespace

VfsInfo vfs_stat(const std::string& path) {
  VfsInfo out;
  if (is_drive_uri(path)) {
    DriveStat st;
    std::string err;
    if (!drive_stat_uri(path, st, err) || !st.exists) return out;
    out.exists = true;
    out.is_dir = st.is_dir;
    out.size = st.size;
    out.mtime = st.mtime;
    return out;
  }
  if (!is_remote_uri(path)) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return out;
    out.exists = true;
    out.is_dir = S_ISDIR(st.st_mode);
    out.size = out.is_dir ? 0 : static_cast<uint64_t>(st.st_size);
    out.mtime = st.st_mtime;
    return out;
  }
  GFile* f = vfs_gfile(path);
  GError* err = nullptr;
  GFileInfo* info = g_file_query_info(
      f, "standard::type,standard::size,time::modified",
      G_FILE_QUERY_INFO_NONE, nullptr, &err);
  if (err) g_error_free(err);
  if (!info) {
    g_object_unref(f);
    return out;
  }
  out.exists = true;
  out.is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
  if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    out.size = out.is_dir ? 0 : g_file_info_get_size(info);
  GDateTime* dt = g_file_info_get_modification_date_time(info);
  if (dt) {
    out.mtime = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
  }
  g_object_unref(info);
  g_object_unref(f);
  return out;
}

bool vfs_exists(const std::string& path) { return vfs_stat(path).exists; }

bool vfs_is_dir(const std::string& path) {
  VfsInfo st = vfs_stat(path);
  return st.exists && st.is_dir;
}

bool vfs_equivalent(const std::string& a, const std::string& b) {
  bool ad = is_drive_uri(a), bd = is_drive_uri(b);
  if (ad || bd) {
    if (!ad || !bd) return false;
    return drive_normalize(a) == drive_normalize(b);
  }
  bool ar = is_remote_uri(a), br = is_remote_uri(b);
  if (ar != br) return false;
  if (ar) return remote_normalize(a) == remote_normalize(b);
  std::error_code ec;
  return fs::equivalent(a, b, ec);
}

bool vfs_remove_all(const std::string& path) {
  if (is_drive_uri(path)) {
    // Permanent delete (parity with local fs::remove_all); the trash flow
    // goes through drive_trash_path instead.
    std::string err;
    return drive_delete_path(path, err);
  }
  if (!is_remote_uri(path)) {
    std::error_code ec;
    return fs::remove_all(path, ec) > 0 || !ec;
  }
  // Remote: delete children first (depth-first), then the node itself.
  std::vector<std::string> dirs{path};
  std::vector<std::string> stack{path};
  while (!stack.empty()) {
    std::string cur = std::move(stack.back());
    stack.pop_back();
    GFile* dir = vfs_gfile(cur);
    GError* err = nullptr;
    GFileEnumerator* en = g_file_enumerate_children(
        dir, "standard::name,standard::type", G_FILE_QUERY_INFO_NONE, nullptr, &err);
    if (err) g_error_free(err);
    if (!en) {
      g_object_unref(dir);
      // Unlistable node: try deleting directly (may be a file).
      GFile* f = vfs_gfile(cur);
      bool gone = g_file_delete(f, nullptr, nullptr);
      g_object_unref(f);
      if (cur == path) return gone;
      if (!gone) return false;
      continue;
    }
    if (cur != path) dirs.push_back(cur);
    for (;;) {
      GError* nerr = nullptr;
      GFileInfo* info = g_file_enumerator_next_file(en, nullptr, &nerr);
      if (nerr) g_error_free(nerr);
      if (!info) break;
      const char* nm = g_file_info_get_name(info);
      if (nm && *nm && std::string(nm) != "." && std::string(nm) != "..") {
        GFile* child = g_file_get_child(dir, nm);
        char* uri = child ? g_file_get_uri(child) : nullptr;
        bool child_dir =
            g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        if (child_dir && uri)
          stack.push_back(uri);
        else if (uri) {
          GFile* f = vfs_gfile(uri);
          g_file_delete(f, nullptr, nullptr);
          g_object_unref(f);
        }
        if (uri) g_free(uri);
        if (child) g_object_unref(child);
      }
      g_object_unref(info);
    }
    GError* cerr = nullptr;
    g_file_enumerator_close(en, nullptr, &cerr);
    if (cerr) g_error_free(cerr);
    g_object_unref(dir);
  }
  // Deepest first.
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
    GFile* f = vfs_gfile(*it);
    bool gone = g_file_delete(f, nullptr, nullptr);
    g_object_unref(f);
    if (!gone) return false;
  }
  return true;
}

bool vfs_rename(const std::string& from, const std::string& to) {
  if (is_drive_uri(from) || is_drive_uri(to)) {
    if (!is_drive_uri(from) || !is_drive_uri(to)) return false;
    std::string err;
    return drive_move_path(from, to, err);
  }
  if (!is_remote_uri(from) && !is_remote_uri(to)) {
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
  }
  GFile* s = vfs_gfile(from);
  GFile* d = vfs_gfile(to);
  GError* err = nullptr;
  bool ok = !!g_file_move(s, d, G_FILE_COPY_NONE, nullptr, nullptr, nullptr, &err);
  if (err) g_error_free(err);
  g_object_unref(s);
  g_object_unref(d);
  return ok;
}

std::string vfs_rename_entry(const std::string& path, const std::string& new_name) {
  if (new_name.empty() || new_name.find('/') != std::string::npos) return {};
  std::string dest;
  if (is_drive_uri(path)) {
    std::string parent = drive_parent(path);
    if (parent.empty()) return {};
    if (parent.back() != '/') parent += '/';
    // Escape: Drive names may contain '/' (vfs_rename_entry already
    // rejects '/' in the new name, so this is just encoding hygiene).
    gchar* esc = g_uri_escape_string(new_name.c_str(), "", TRUE);
    dest = parent + (esc ? esc : new_name);
    g_free(esc);
  } else if (is_remote_uri(path)) {
    std::string parent = remote_parent(path);
    if (parent.empty()) return {};
    if (parent.back() != '/') parent += '/';
    dest = parent + new_name;
  } else {
    dest = (fs::path(path).parent_path() / new_name).string();
  }
  return vfs_rename(path, dest) ? dest : std::string();
}

bool vfs_mkdir(const std::string& path) {
  if (is_drive_uri(path)) {
    std::string err;
    return drive_mkdir_path(path, err);
  }
  if (!is_remote_uri(path)) {
    std::error_code ec;
    return fs::create_directory(path, ec) && !ec;
  }
  GFile* f = vfs_gfile(path);
  GError* err = nullptr;
  bool ok = !!g_file_make_directory(f, nullptr, &err);
  if (err) g_error_free(err);
  g_object_unref(f);
  return ok;
}

bool vfs_create_empty(const std::string& path) {
  if (is_drive_uri(path)) {
    // MIME follows the name (text/plain default); the file is truly empty.
    std::string mime = mime_by_ext(path);
    if (mime.empty()) mime = "text/plain";
    std::string err;
    return drive_create_empty_path(path, mime, err);
  }
  if (!is_remote_uri(path)) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fclose(f);
    return true;
  }
  GFile* f = vfs_gfile(path);
  GError* err = nullptr;
  GFileOutputStream* out =
      g_file_create(f, G_FILE_CREATE_NONE, nullptr, &err);
  if (err) g_error_free(err);
  if (out) {
    GError* cerr = nullptr;
    g_output_stream_close(G_OUTPUT_STREAM(out), nullptr, &cerr);
    if (cerr) g_error_free(cerr);
    g_object_unref(out);
  }
  g_object_unref(f);
  return out != nullptr;
}

bool vfs_symlink(const std::string& target, const std::string& link_path,
                 bool /*target_is_dir*/) {
  if (!is_remote_uri(link_path)) {
    std::error_code ec;
    fs::create_symlink(target, link_path, ec);
    return !ec;
  }
  // Symlink targets on servers are server-side paths; pass through as-is.
  GFile* f = vfs_gfile(link_path);
  GError* err = nullptr;
  bool ok = !!g_file_make_symbolic_link(f, target.c_str(), nullptr, &err);
  if (err) g_error_free(err);
  g_object_unref(f);
  return ok;
}


namespace {

struct FileTask {
  std::string src;
  std::string dst;
};

void collect_remote_tasks(const std::string& src, const std::string& dst,
                          std::vector<FileTask>& out, uint64_t& bytes) {
  // Enumerate src (either world) and flatten to file tasks + dir creations.
  // Directories are created inline during the walk below; this pass only
  // counts files/bytes for progress.
  std::vector<std::pair<std::string, std::string>> stack{{src, dst}};
  while (!stack.empty()) {
    auto [s, d] = std::move(stack.back());
    stack.pop_back();
    bool src_remote = is_remote_uri(s);
    if (src_remote) {
      GFile* dir = vfs_gfile(s);
      GError* err = nullptr;
      GFileEnumerator* en = g_file_enumerate_children(
          dir, "standard::name,standard::type,standard::size",
          G_FILE_QUERY_INFO_NONE, nullptr, &err);
      if (err) g_error_free(err);
      if (!en) {
        // Single file (or unreadable): count as one file task.
        VfsInfo st = vfs_stat(s);
        if (st.exists && !st.is_dir) {
          out.push_back({s, d});
          bytes += st.size;
        }
        g_object_unref(dir);
        continue;
      }
      for (;;) {
        GError* nerr = nullptr;
        GFileInfo* info = g_file_enumerator_next_file(en, nullptr, &nerr);
        if (nerr) g_error_free(nerr);
        if (!info) break;
        const char* nm = g_file_info_get_name(info);
        bool child_dir =
            g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
        uint64_t sz = 0;
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
          sz = g_file_info_get_size(info);
        if (nm && *nm) {
          GFile* child = g_file_get_child(dir, nm);
          char* child_uri = child ? g_file_get_uri(child) : nullptr;
          std::string cs = child_uri ? child_uri : (s + "/" + nm);
          std::string cd = d + "/" + nm;
          if (child_dir) {
            stack.emplace_back(cs, cd);
          } else {
            out.push_back({cs, cd});
            bytes += sz;
          }
          if (child_uri) g_free(child_uri);
          if (child) g_object_unref(child);
        }
        g_object_unref(info);
      }
      GError* cerr = nullptr;
      g_file_enumerator_close(en, nullptr, &cerr);
      if (cerr) g_error_free(cerr);
      g_object_unref(dir);
    } else {
      std::error_code ec;
      if (fs::is_directory(s, ec)) {
        for (auto& de :
             fs::recursive_directory_iterator(s, ec)) {
          if (de.is_regular_file(ec)) {
            std::error_code ec2;
            std::string rel = fs::relative(de.path(), s, ec2).string();
            if (ec2) continue;
            out.push_back({de.path().string(), (fs::path(d) / rel).string()});
            bytes += de.file_size(ec2);
          }
        }
      } else if (fs::exists(s, ec)) {
        out.push_back({s, d});
        bytes += fs::file_size(s, ec);
      }
    }
  }
}

struct CopyProgress {
  OperationProgress* prog = nullptr;
  uint64_t last = 0;
};

void copy_progress_cb(goffset current, goffset /*total*/, gpointer data) {
  auto* cx = static_cast<CopyProgress*>(data);
  uint64_t cur = static_cast<uint64_t>(current);
  if (cur > cx->last && cx->prog) {
    cx->prog->done_bytes.fetch_add(cur - cx->last);
    cx->last = cur;
  }
}

// Fire-and-forget mount for op paths: mounts are normally already cached
// from browsing, but an op can target a never-visited location (or a stale
// mount). Any interactive prompt aborts — ops never block on auth UI.
struct EnsureCtx {
  GMainLoop* loop = nullptr;
  bool done = false;
  bool ok = false;
  std::string err;
};

void on_ensure_done(GObject* src, GAsyncResult* res, gpointer data) {
  auto* ec = static_cast<EnsureCtx*>(data);
  GError* err = nullptr;
  ec->ok = !!g_file_mount_enclosing_volume_finish(G_FILE(src), res, &err);
  if (!ec->ok && err &&
      g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
    // Already mounted is success as far as copying is concerned.
    ec->ok = true;
  }
  if (!ec->ok) ec->err = (err && err->message) ? err->message : "mount failed";
  if (err) g_error_free(err);
  ec->done = true;
  g_main_loop_quit(ec->loop);
}

void on_ensure_ask(GMountOperation* op, gpointer data) {
  auto* ec = static_cast<EnsureCtx*>(data);
  ec->ok = false;
  ec->err = "Authentication required — open the location first";
  ec->done = true;
  g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
  g_main_loop_quit(ec->loop);
}

void on_ensure_ask_pw(GMountOperation* op, char*, char*, char*,
                      GAskPasswordFlags, gpointer data) {
  on_ensure_ask(op, data);
}

void on_ensure_ask_q(GMountOperation* op, char*, char**, gpointer data) {
  on_ensure_ask(op, data);
}

bool remote_ensure_mounted(const std::string& uri, GCancellable* canc,
                           std::string& err_out) {
  GMainContext* ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);
  GMainLoop* loop = g_main_loop_new(ctx, FALSE);
  GFile* root = g_file_new_for_uri(uri.c_str());
  GMountOperation* op = g_mount_operation_new();
  EnsureCtx ec;
  ec.loop = loop;
  gulong h1 = g_signal_connect(op, "ask-password", G_CALLBACK(on_ensure_ask_pw), &ec);
  gulong h2 = g_signal_connect(op, "ask-question", G_CALLBACK(on_ensure_ask_q), &ec);
  (void)h1;
  (void)h2;
  g_file_mount_enclosing_volume(root, G_MOUNT_MOUNT_NONE, op, canc,
                                on_ensure_done, &ec);
  g_main_loop_run(loop);
  bool ok = ec.done && ec.ok;
  if (!ok && err_out.empty()) err_out = ec.err.empty() ? "mount failed" : ec.err;
  g_object_unref(op);
  g_object_unref(root);
  g_main_loop_unref(loop);
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  return ok;
}

} // namespace

void remote_do_copy_move(std::vector<std::string> src_paths,
                         std::string dest_dir, bool is_move,
                         std::shared_ptr<OperationProgress> prog,
                         std::function<void(bool cancelled)> on_complete,
                         std::vector<std::string> allow_overwrite,
                         std::vector<std::string> dst_names,
                         std::shared_ptr<std::string> error_out) {
  if (src_paths.empty()) {
    if (on_complete) on_complete(false);
    return;
  }
  prog->active.store(true);
  prog->cancel.store(false);
  prog->progress.store(0.0);
  prog->copied_files.store(0);
  prog->total_files.store(0);
  prog->success.store(true);

  ThreadPool::instance().enqueue(
      [src_paths = std::move(src_paths), dest_dir = std::move(dest_dir),
       is_move, prog, on_complete = std::move(on_complete),
       allow_overwrite = std::move(allow_overwrite),
       dst_names = std::move(dst_names),
       error_out = std::move(error_out)] {
        prog->start_time = std::chrono::steady_clock::now();
        GCancellable* canc = g_cancellable_new();

        auto cancelled = [&] {
          if (prog->cancel.load()) {
            g_cancellable_cancel(canc);
            return true;
          }
          return false;
        };

        auto fail = [&](const std::string& msg) {
          prog->success.store(false);
          if (error_out && error_out->empty()) *error_out = msg;
        };

        // GIO copies don't auto-mount: ensure every remote side is mounted.
        // Mounts are normally cached from browsing; anything prompting for
        // auth aborts here (open the location first to authenticate).
        {
          std::vector<std::string> roots{dest_dir};
          for (auto& s : src_paths)
            if (is_remote_uri(s)) roots.push_back(s);
          std::sort(roots.begin(), roots.end());
          roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
          for (auto& r : roots) {
            if (cancelled()) break;
            if (!is_remote_uri(r)) continue;
            std::string merr;
            if (!remote_ensure_mounted(r, canc, merr)) {
              if (!g_cancellable_is_cancelled(canc)) fail(merr);
              break;
            }
          }
          if (!prog->success.load() || cancelled()) {
            prog->active.store(false);
            prog->clear_current_file();
            bool was_cancelled =
                prog->cancel.load() || g_cancellable_is_cancelled(canc);
            g_object_unref(canc);
            DeferredCall::callLater([on_complete, was_cancelled] {
              if (on_complete) on_complete(was_cancelled);
            });
            return;
          }
        }

        // Phase 1: flatten + count (mirrors local files/bytes totals).
        std::vector<FileTask> tasks;
        uint64_t total_bytes = 0;
        for (size_t si = 0; si < src_paths.size(); ++si) {
          if (cancelled()) break;
          std::string fname = fs::path(src_paths[si]).filename().string();
          if (si < dst_names.size() && !dst_names[si].empty())
            fname = dst_names[si];
          std::string dest_base = dest_dir;
          if (!dest_base.empty() && dest_base.back() != '/' &&
              !is_remote_uri(dest_base))
            dest_base += '/';
          else if (is_remote_uri(dest_base) && dest_base.back() != '/')
            dest_base += '/';
          collect_remote_tasks(src_paths[si], dest_base + fname, tasks,
                               total_bytes);
        }
        // Directories themselves contribute no file tasks; count them for
        // progress parity with the local engine is skipped (files only).
        prog->total_files.store(static_cast<int>(tasks.size()));
        prog->total_bytes.store(total_bytes);

        int done = 0;
        for (auto& t : tasks) {
          if (cancelled()) break;
          // Same overwrite contract as the local engine: listed per source.
          bool overwrite =
              std::find(allow_overwrite.begin(), allow_overwrite.end(), t.src) !=
              allow_overwrite.end();
          // Ensure parent exists (mkdir -p semantics on both sides).
          {
            std::string parent =
                is_remote_uri(t.dst)
                    ? remote_parent(t.dst)
                    : fs::path(t.dst).parent_path().string();
            if (!parent.empty()) {
              if (is_remote_uri(parent)) {
                // Best-effort parent chain creation.
                std::string norm = remote_normalize(parent);
                auto pos = norm.find("://");
                std::string rest =
                    pos == std::string::npos ? norm : norm.substr(pos + 3);
                std::string cur =
                    pos == std::string::npos ? "" : norm.substr(0, pos + 3);
                size_t start = 0;
                while (start < rest.size()) {
                  size_t slash = rest.find('/', start);
                  std::string comp = (slash == std::string::npos)
                                         ? rest.substr(start)
                                         : rest.substr(start, slash - start);
                  if (!comp.empty()) {
                    if (!cur.empty() && cur.back() != '/') cur += '/';
                    cur += comp;
                    vfs_mkdir(cur);
                  }
                  if (slash == std::string::npos) break;
                  start = slash + 1;
                }
              } else {
                std::error_code ec;
                fs::create_directories(parent, ec);
              }
            }
          }
          prog->set_current_file(fs::path(t.src).filename().string());
          GFile* s = vfs_gfile(t.src);
          GFile* d = vfs_gfile(t.dst);
          // Same-filesystem move: fast rename first.
          bool moved = false;
          if (is_move) {
            bool same_world =
                is_remote_uri(t.src) == is_remote_uri(t.dst);
            bool same_mount = same_world;
            if (same_world && is_remote_uri(t.src)) {
              auto strip = [](const std::string& u) {
                auto p = u.find("://");
                std::string r = p == std::string::npos ? u : u.substr(p + 3);
                auto slash = r.find('/');
                return r.substr(0, slash);
              };
              same_mount = strip(t.src) == strip(t.dst);
            }
            if (same_mount) {
              GError* merr = nullptr;
              moved = !!g_file_move(
                  s, d, overwrite ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_NONE,
                  canc, nullptr, nullptr, &merr);
              if (merr) g_error_free(merr);
            }
          }
          if (!moved) {
            CopyProgress cx{prog.get(), 0};
            GError* cerr = nullptr;
            bool ok = !!g_file_copy(
                s, d, overwrite ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_NONE,
                canc, copy_progress_cb, &cx, &cerr);
            if (cerr) g_error_free(cerr);
            if (ok) {
              ++done;
              prog->copied_files.store(done);
              prog->progress.store(!tasks.empty()
                                       ? static_cast<double>(done) / tasks.size()
                                       : 0.0);
              if (is_move) {
                // Best-effort source removal (mirror local copy+delete).
                if (is_remote_uri(t.src)) {
                  GFile* del = vfs_gfile(t.src);
                  GError* derr = nullptr;
                  g_file_delete(del, canc, &derr);
                  if (derr) g_error_free(derr);
                  g_object_unref(del);
                } else {
                  std::error_code ec;
                  fs::remove(t.src, ec);
                }
              }
            } else {
              prog->success.store(false);
            }
          } else {
            ++done;
            prog->copied_files.store(done);
            prog->progress.store(!tasks.empty()
                                     ? static_cast<double>(done) / tasks.size()
                                     : 0.0);
          }
          g_object_unref(s);
          g_object_unref(d);
        }

        prog->active.store(false);
        prog->clear_current_file();
        bool was_cancelled = prog->cancel.load() || g_cancellable_is_cancelled(canc);
        g_object_unref(canc);
        DeferredCall::callLater([on_complete, was_cancelled] {
          if (on_complete) on_complete(was_cancelled);
        });
      });
}

void remote_auth_submit(AppState& app, int choice) {
  RemoteAuthResult res;
  bool submitted = false;
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (app.remote_auth.active && app.remote_auth.promise) {
      if (!app.remote_auth.choices.empty()) {
        if (choice >= 0 &&
            choice < static_cast<int>(app.remote_auth.choices.size())) {
          res.ok = true;
          res.choice = choice;
          submitted = true;
        }
      } else {
        res.ok = true;
        res.username =
            app.remote_auth.need_user ? app.remote_auth_user_buf : std::string();
        res.password = app.remote_auth_pass_buf;
        submitted = true;
      }
      if (submitted) {
        try {
          app.remote_auth.promise->set_value(std::move(res));
        } catch (...) {
          submitted = false;
        }
      }
    }
  }
  (void)submitted;
  app.remote_auth_open = false;
  app.remote_auth_pass_buf.clear();
  app.remote_auth_hover_btn = -1;
}

void remote_auth_cancel(AppState& app) {
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (app.remote_auth.promise) {
      try {
        app.remote_auth.promise->set_value(RemoteAuthResult{});
      } catch (...) {
      }
      app.remote_auth.promise.reset();
    }
    app.remote_auth.active = false;
  }
  app.remote_auth_open = false;
  app.remote_auth_user_buf.clear();
  app.remote_auth_pass_buf.clear();
  app.remote_auth_hover_btn = -1;
}

namespace {

struct MountCtx {
  GMainLoop* loop = nullptr;
  bool done = false;
  bool mount_ok = false;
  std::string mount_err;
  // Deferred interactive request: the reply is withheld until the worker
  // (outside the loop) collects the user's answer. held_op carries the
  // extra ref released after replying.
  bool want_pw = false;
  GAskPasswordFlags pw_flags = static_cast<GAskPasswordFlags>(0);
  std::string pw_message;
  std::string pw_user;
  bool want_q = false;
  std::string q_message;
  std::vector<std::string> q_choices;
  GMountOperation* held_op = nullptr;
};

void on_ask_password(GMountOperation* op, char* message, char* default_user,
                     char* /*default_domain*/, GAskPasswordFlags flags,
                     gpointer data) {
  auto* mc = static_cast<MountCtx*>(data);
  mc->want_pw = true;
  mc->pw_flags = flags;
  if (message) mc->pw_message = message;
  if (default_user) mc->pw_user = default_user;
  if (mc->held_op) g_object_unref(mc->held_op);
  mc->held_op = static_cast<GMountOperation*>(g_object_ref(op));
  g_main_loop_quit(mc->loop);
}

void on_ask_question(GMountOperation* op, char* message, char** choices,
                     gpointer data) {
  auto* mc = static_cast<MountCtx*>(data);
  mc->want_q = true;
  if (message) mc->q_message = message;
  mc->q_choices.clear();
  if (choices) {
    for (int i = 0; choices[i]; ++i) mc->q_choices.emplace_back(choices[i]);
  }
  if (mc->held_op) g_object_unref(mc->held_op);
  mc->held_op = static_cast<GMountOperation*>(g_object_ref(op));
  g_main_loop_quit(mc->loop);
}

void on_mount_done(GObject* src, GAsyncResult* res, gpointer data) {
  auto* mc = static_cast<MountCtx*>(data);
  GError* err = nullptr;
  mc->mount_ok = !!g_file_mount_enclosing_volume_finish(G_FILE(src), res, &err);
  if (!mc->mount_ok && err &&
      g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
    // Already mounted is success as far as listing is concerned.
    mc->mount_ok = true;
  }
  if (!mc->mount_ok)
    mc->mount_err = (err && err->message) ? err->message : "mount failed";
  if (err) g_error_free(err);
  mc->done = true;
  g_main_loop_quit(mc->loop);
}

// Publish an interactive request for the UI thread and open the dialog.
// Returns false when already superseded (caller must abort quietly).
bool request_remote_auth(AppState* ap, uint64_t gen, MountCtx& mc) {
  std::lock_guard<std::mutex> lk(ap->scan_mtx);
  if (ap->scan_generation != gen || ap->scan_cancel.load()) return false;
  RemoteAuthRequest req;
  req.active = true;
  req.gen = gen;
  req.promise = std::make_shared<std::promise<RemoteAuthResult>>();
  if (mc.want_q) {
    req.message = mc.q_message.empty() ? "The server asks for approval." : mc.q_message;
    req.choices = mc.q_choices;
    req.need_password = false;
  } else {
    req.message = mc.pw_message.empty() ? "Authentication required." : mc.pw_message;
    req.username = mc.pw_user;
    req.need_user = (mc.pw_flags & G_ASK_PASSWORD_NEED_USERNAME) != 0;
    req.need_password = (mc.pw_flags & G_ASK_PASSWORD_NEED_PASSWORD) != 0;
    if (!req.need_user && !req.need_password) req.need_password = true;
  }
  ap->remote_auth = std::move(req);
  // Prime the transient dialog state (single UI source of truth below).
  ap->remote_auth_open = true;
  ap->remote_auth_user_buf = ap->remote_auth.username;
  ap->remote_auth_pass_buf.clear();
  ap->remote_auth_focus = ap->remote_auth.need_user ? 0 : 1;
  if (ap->remote_auth.need_user && !ap->remote_auth.need_password)
    ap->remote_auth_focus = 0;
  ap->remote_auth_hover_btn = -1;
  DeferredCall::callLater([ap]() { draw(*ap); });
  return true;
}

// Block (in sliced waits) for the user's answer. Stale generations and
// cancellation resolve as abort. Never throws.
RemoteAuthResult wait_remote_auth(AppState* ap, uint64_t gen) {
  std::shared_future<RemoteAuthResult> fut;
  {
    std::lock_guard<std::mutex> lk(ap->scan_mtx);
    if (!ap->remote_auth.promise) return {};
    fut = ap->remote_auth.promise->get_future().share();
  }
  for (;;) {
    if (fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
      RemoteAuthResult r;
      try {
        r = fut.get();
      } catch (...) {
      }
      return r;
    }
    bool stale = false;
    {
      std::lock_guard<std::mutex> lk(ap->scan_mtx);
      stale = ap->scan_generation != gen || ap->scan_cancel.load();
    }
    if (stale) {
      // Superseded: satisfy the promise so nobody waits on it, then abort.
      std::lock_guard<std::mutex> lk(ap->scan_mtx);
      if (ap->remote_auth.promise) {
        try {
          ap->remote_auth.promise->set_value(RemoteAuthResult{});
        } catch (...) {
        }
        ap->remote_auth.promise.reset();
      }
      ap->remote_auth.active = false;
      ap->remote_auth_open = false;
      return {};
    }
  }
}

void drop_held_op(MountCtx& mc) {
  if (mc.held_op) {
    g_object_unref(mc.held_op);
    mc.held_op = nullptr;
  }
  mc.want_pw = false;
  mc.want_q = false;
}

} // namespace

void open_connect_dialog(AppState& app) {
  // Keep host/user/path/port for convenience; passwords never persist.
  app.connect_pass.clear();
  app.connect_port_buf = std::to_string(app.connect_port > 0 ? app.connect_port : 22);
  app.connect_focus = 0;
  app.connect_hover_btn = -1;
  app.connect_open = true;
}

static std::string connect_trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

void connect_submit(AppState& app) {
  std::string host = connect_trim(app.connect_host);
  if (host.empty()) {
    app.operation_status = "Enter a host name";
    app.operation_status_expires_ms = menu_expiry_3s();
    return;
  }
  int port = 22;
  {
    std::string digits;
    for (char c : app.connect_port_buf)
      if (c >= '0' && c <= '9') digits += c;
    if (!digits.empty()) port = std::clamp(std::stoi(digits), 1, 65535);
  }
  app.connect_port = port;
  std::string user = connect_trim(app.connect_user);
  std::string path = connect_trim(app.connect_path);
  if (path.empty()) path = "/";
  std::string uri = build_sftp_uri(host, user, port, path);
  // Upsert bookmark (credentials never persisted).
  auto& servers = app.remote_servers;
  auto it = std::find_if(servers.begin(), servers.end(),
                         [&](const AppState::RemoteServer& s) {
                           return s.host == host && s.user == user && s.port == port;
                         });
  if (it == servers.end()) {
    AppState::RemoteServer srv;
    srv.host = host;
    srv.user = user;
    srv.path = path;
    srv.port = port;
    servers.push_back(std::move(srv));
  } else {
    it->path = path;
  }
  save_file_browser_settings(app);
  refresh_sidebar(app);
  app.remote_pending_password = app.connect_pass;
  app.connect_open = false;
  app.connect_pass.clear(); // session-only: never kept past the dialog
  app.connect_hover_btn = -1;
  navigate_to(app, uri);
}

// Out-of-process listing via horizon-vfs(1). Fills entries pre-sorted.
// Returns 0 = helper listed ok, 1 = fall back to the interactive in-process
// flow (helper missing/failed/auth-required), -1 = cancelled (stay silent).
// Session passwords travel over a pipe and are scrubbed after use.
static int remote_list_via_helper(AppState* ap, uint64_t gen,
                                  const std::string& uri,
                                  const std::string& password,
                                  std::vector<FileEntry>& out) {
  if (const char* e = std::getenv("EH_VFS_HELPER"))
    if (*e && e[0] == '0') return 1;
  const char* bin = std::getenv("EH_VFS_HELPER_BIN");
  if (!bin) bin = "horizon-vfs";

  auto cancelled = [&] {
    std::lock_guard<std::mutex> lk(ap->scan_mtx);
    return ap->scan_generation != gen || ap->scan_cancel.load();
  };

  int outfds[2] = {-1, -1}, errfds[2] = {-1, -1}, infds[2] = {-1, -1};
  if (::pipe(outfds) != 0) return 1;
  if (::pipe(errfds) != 0) {
    ::close(outfds[0]);
    ::close(outfds[1]);
    return 1;
  }
  bool use_pw = !password.empty();
  if (use_pw && ::pipe(infds) != 0) {
    ::close(outfds[0]);
    ::close(outfds[1]);
    ::close(errfds[0]);
    ::close(errfds[1]);
    return 1;
  }
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(outfds[0]);
    ::close(outfds[1]);
    ::close(errfds[0]);
    ::close(errfds[1]);
    if (use_pw) {
      ::close(infds[0]);
      ::close(infds[1]);
    }
    return 1;
  }
  if (pid == 0) {
    ::dup2(outfds[1], STDOUT_FILENO);
    ::dup2(errfds[1], STDERR_FILENO);
    ::close(outfds[0]);
    ::close(outfds[1]);
    ::close(errfds[0]);
    ::close(errfds[1]);
    if (use_pw) {
      ::dup2(infds[0], STDIN_FILENO);
      ::close(infds[0]);
      ::close(infds[1]);
      ::execlp(bin, "horizon-vfs", "list", uri.c_str(), "--password-stdin",
               (char*)nullptr);
    } else {
      ::execlp(bin, "horizon-vfs", "list", uri.c_str(), (char*)nullptr);
    }
    _exit(127);
  }
  ::close(outfds[1]);
  ::close(errfds[1]);
  if (use_pw) {
    ::close(infds[0]);
    ssize_t off = 0;
    std::string pwline = password + "\n";
    while (off < (ssize_t)pwline.size()) {
      ssize_t n =
          ::write(infds[1], pwline.data() + off, pwline.size() - off);
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      off += n;
    }
    ::close(infds[1]);
    volatile char* scrub = pwline.data();
    for (size_t i = 0; i < pwline.size(); ++i) scrub[i] = 0;
  }
  int fl = ::fcntl(outfds[0], F_GETFL, 0);
  if (fl >= 0) ::fcntl(outfds[0], F_SETFL, fl | O_NONBLOCK);

  std::string buf;
  buf.reserve(32768);
  char tmp[16384];
  auto handle_record = [&](const char* beg, const char* end) {
    const char* tabs[11] = {};
    const char* p = beg;
    for (int i = 0; i < 11; ++i) {
      p = static_cast<const char*>(std::memchr(p, '\t', end - p));
      if (!p) return;
      tabs[i] = p++;
    }
    FileEntry e;
    e.name.assign(beg, tabs[0]);
    e.path.assign(tabs[0] + 1, tabs[1]);
    e.is_dir = tabs[1][1] == '1';
    e.is_symlink = tabs[2][1] == '1';
    e.size = e.is_dir ? 0 : std::strtoull(tabs[3] + 1, nullptr, 10);
    e.modified_sec = std::strtoll(tabs[4] + 1, nullptr, 10);
    e.mime_type.assign(tabs[5] + 1, tabs[6]);
    e.is_hidden = !e.name.empty() && e.name[0] == '.';
    e.readable = tabs[7][1] == '1';
    e.writable = tabs[8][1] == '1';
    e.link_target.assign(tabs[9] + 1, tabs[10]);
    e.extension.assign(tabs[10] + 1, end);
    for (auto& c : e.extension)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    e.type = detect_file_type_for_path(e.name, e.is_dir, "");
    out.push_back(std::move(e));
  };

  bool saw_eof = false;
  int status = 0;
  bool child_done = false;
  for (;;) {
    if (cancelled()) {
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      ::close(outfds[0]);
      ::close(errfds[0]);
      return -1;
    }
    struct pollfd pfd{};
    pfd.fd = outfds[0];
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, 50);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      ssize_t n = ::read(outfds[0], tmp, sizeof(tmp));
      if (n > 0) {
        buf.append(tmp, static_cast<size_t>(n));
        size_t pos = 0;
        for (;;) {
          size_t z = buf.find('\0', pos);
          if (z == std::string::npos) break;
          handle_record(buf.data() + pos, buf.data() + z);
          pos = z + 1;
        }
        buf.erase(0, pos);
      } else if (n == 0) {
        saw_eof = true;
      }
    }
    pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) child_done = true;
    if (saw_eof && child_done) break;
    if (child_done && !(pfd.revents & POLLIN)) {
      // Child exited; drain anything left then stop.
      ssize_t n = ::read(outfds[0], tmp, sizeof(tmp));
      if (n > 0) {
        buf.append(tmp, static_cast<size_t>(n));
        size_t pos = 0;
        for (;;) {
          size_t z = buf.find('\0', pos);
          if (z == std::string::npos) break;
          handle_record(buf.data() + pos, buf.data() + z);
          pos = z + 1;
        }
        buf.erase(0, pos);
      } else {
        break;
      }
    }
  }
  ::close(outfds[0]);
  std::string errbuf;
  char ebuf[1024];
  for (;;) {
    ssize_t n = ::read(errfds[0], ebuf, sizeof(ebuf));
    if (n > 0)
      errbuf.append(ebuf, static_cast<size_t>(n));
    else
      break;
  }
  ::close(errfds[0]);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
  if (!cancelled() && WIFEXITED(status) && WEXITSTATUS(status) == 127 &&
      out.empty())
    return 1;  // helper missing
  // Auth-required (3) and hard errors (1) both fall back: auth needs the
  // interactive dialogs, errors get re-reported by the in-process attempt.
  // Seed the status with the helper's message in case fallback is skipped.
  (void)errbuf;
  return 1;
}

void reload_remote_dir(AppState& app) {  app.scan_target_pane = app.active_pane ? 1 : 0;
  reset_preview(app);
  hide_tooltip(app);

  std::string uri = remote_normalize(app.cur_tab().current_path);
  app.cur_tab().current_path = uri;

  if (!remote_scheme_available(scheme_of(uri))) {
    app.operation_status = "Remote support missing (install gvfs-backends)";
    app.operation_status_expires_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count() +
        3000;
    draw(app);
    return;
  }

  remote_cancel_inflight(app);
  join_scan(app);

  const uint64_t gen = ++app.scan_generation;
  app.scan_cancel.store(false);
  app.scan_ready_flag.store(false);
  app.scan_active_path = uri;
  // Reuse the local sort params so the apply path stays identical; remote
  // ordering itself is folders-first + case-insensitive name (slice 1).
  ScanParams sp;
  app.scan_active_params = sp;

  // Grab one-shot credentials (session-only, never persisted).
  std::string password;
  bool folders_first = app.folders_before_files;
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    password = std::move(app.remote_pending_password);
    app.remote_pending_password.clear();
    if (app.remote_cancellable) g_object_unref(app.remote_cancellable);
    app.remote_cancellable = g_cancellable_new();
  }

  AppState* ap = &app;
  app.scan_thread = std::thread([ap, gen, uri, password, folders_first]() {
    std::vector<FileEntry> v;
    std::string err;

    // Prefer the out-of-process GIO listing so mount threads + GFileInfo
    // pressure live outside horizon-files RSS. Auth-required, errors and a
    // missing helper all fall through to the interactive in-process flow.
    int helper_rc = remote_list_via_helper(ap, gen, uri, password, v);
    if (helper_rc == 0) {
      std::lock_guard<std::mutex> lk(ap->scan_mtx);
      if (ap->scan_generation != gen || ap->scan_cancel.load()) return;
      ap->scan_result.error.clear();
      ap->scan_result.generation = gen;
      ap->scan_result.path = uri;
      ap->scan_result.entries = std::move(v);
      ap->scan_ready_flag.store(true, std::memory_order_release);
      return;
    }
    if (helper_rc < 0) return;  // cancelled mid-helper: stay silent
    v.clear();

    GCancellable* canc = nullptr;
    {
      std::lock_guard<std::mutex> lk(ap->scan_mtx);
      canc = ap->remote_cancellable ? static_cast<GCancellable*>(g_object_ref(ap->remote_cancellable)) : nullptr;
    }

    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);

    GFile* root = g_file_new_for_uri(uri.c_str());
    GMountOperation* op = g_mount_operation_new();
    g_mount_operation_set_anonymous(op, FALSE);
    if (!password.empty()) {
      g_mount_operation_set_password(op, password.c_str());
      g_mount_operation_set_password_save(op, G_PASSWORD_SAVE_NEVER);
    }
    MountCtx mc;
    mc.loop = loop;
    g_signal_connect(op, "ask-password", G_CALLBACK(on_ask_password), &mc);
    g_signal_connect(op, "ask-question", G_CALLBACK(on_ask_question), &mc);

    // Mount attempt loop: a SINGLE mount call runs for the whole flow; each
    // loop iteration runs the main loop until the mount either completes or
    // asks something interactively. Interactive answers are collected
    // off-loop (see request/wait_remote_auth), applied to the held op, and
    // the loop re-runs so the SAME mount continues. Never re-issue the
    // mount per round: concurrent mounts for one listing race each other.
    bool ok = false;
    bool aborted = false; // user cancel / superseded: stay silent
    g_file_mount_enclosing_volume(root, G_MOUNT_MOUNT_NONE, op, canc,
                                  on_mount_done, &mc);
    static constexpr int kMaxAuthRounds = 6;
    for (int round = 0; round < kMaxAuthRounds; ++round) {
      mc.done = false;
      mc.mount_ok = false;
      mc.mount_err.clear();
      mc.want_pw = false;
      mc.want_q = false;
      g_main_loop_run(loop);
      if (mc.done && mc.mount_ok) {
        ok = true;
        break;
      }
      if (mc.done && !mc.mount_ok && !mc.want_pw && !mc.want_q) {
        err = mc.mount_err.empty() ? "mount failed" : mc.mount_err;
        break; // hard error, nothing to ask
      }
      // Interactive request outstanding: publish it and wait off-loop.
      bool was_pw = mc.want_pw;
      if (!request_remote_auth(ap, gen, mc)) {
        if (mc.held_op) {
          g_mount_operation_reply(mc.held_op, G_MOUNT_OPERATION_ABORTED);
          drop_held_op(mc);
        }
        err.clear(); // superseded by a newer listing; stay silent
        aborted = true;
        break;
      }
      RemoteAuthResult res = wait_remote_auth(ap, gen);
      {
        std::lock_guard<std::mutex> lk(ap->scan_mtx);
        ap->remote_auth.active = false;
        ap->remote_auth_open = false;
        ap->remote_auth.promise.reset();
      }
      DeferredCall::callLater([ap]() { draw(*ap); });
      bool stale = false;
      {
        std::lock_guard<std::mutex> lk(ap->scan_mtx);
        stale = ap->scan_generation != gen || ap->scan_cancel.load();
      }
      if (!res.ok || stale) {
        if (mc.held_op) {
          g_mount_operation_reply(mc.held_op, G_MOUNT_OPERATION_ABORTED);
          drop_held_op(mc);
        }
        err.clear(); // user cancel / superseded: stay silent
        aborted = true;
        break;
      }
      if (mc.held_op) {
        if (was_pw) {
          if (!res.username.empty())
            g_mount_operation_set_username(mc.held_op, res.username.c_str());
          g_mount_operation_set_password(mc.held_op, res.password.c_str());
          g_mount_operation_set_password_save(mc.held_op, G_PASSWORD_SAVE_NEVER);
        } else {
          g_mount_operation_set_choice(mc.held_op, res.choice);
        }
        g_mount_operation_reply(mc.held_op, G_MOUNT_OPERATION_HANDLED);
        drop_held_op(mc);
      }
      // Loop around: the mount continues and either completes or asks again.
    }
    if (!ok && !aborted && err.empty()) err = "Too many authentication attempts";
    if (ok) {
      GError* eerr = nullptr;
      GFileEnumerator* en = g_file_enumerate_children(
          root, kChildAttrs, G_FILE_QUERY_INFO_NONE, canc, &eerr);
      if (!en) {
        ok = false;
        err = eerr && eerr->message ? eerr->message : "cannot list folder";
        if (eerr) g_error_free(eerr);
      } else {
        for (;;) {
          if (ap->scan_cancel.load(std::memory_order_acquire) ||
              (canc && g_cancellable_is_cancelled(canc)))
            break;
          GError* nerr = nullptr;
          GFileInfo* info = g_file_enumerator_next_file(en, canc, &nerr);
          if (nerr) {
            if (!g_error_matches(nerr, G_IO_ERROR, G_IO_ERROR_CANCELLED))
              err = nerr->message ? nerr->message : "listing failed";
            g_error_free(nerr);
            break;
          }
          if (!info) break; // done
          const char* child_name =
              info ? g_file_info_get_name(info) : nullptr;
          if (!child_name || !*child_name) {
            g_object_unref(info);
            continue;
          }
          GFile* child = g_file_get_child(root, child_name);
          char* child_uri = child ? g_file_get_uri(child) : nullptr;
          v.push_back(remote_entry_from_info(
              info, child_uri ? child_uri : uri));
          if (child_uri) g_free(child_uri);
          if (child) g_object_unref(child);
          g_object_unref(info);
        }
        GError* cerr = nullptr;
        g_file_enumerator_close(en, canc, &cerr);
        if (cerr) g_error_free(cerr);
        g_object_unref(en);
      }
      // Folders first, then case-insensitive name order (slice 1).
      std::sort(v.begin(), v.end(),
                [folders_first](const FileEntry& a, const FileEntry& b) {
                  if (folders_first && a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  std::string al = a.name, bl = b.name;
                  for (auto& c : al) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                  for (auto& c : bl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                  if (al != bl) return al < bl;
                  return a.name < b.name;
                });
    }

    g_object_unref(op);
    g_object_unref(root);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    if (canc) g_object_unref(canc);

    {
      std::lock_guard<std::mutex> lk(ap->scan_mtx);
      if (ap->scan_generation != gen || ap->scan_cancel.load()) return;
      // Errors ride in scan_result.error: apply_scan_result surfaces them as
      // a toast instead of clearing them with the previous status.
      ap->scan_result.error = err;
      ap->scan_result.generation = gen;
      ap->scan_result.path = uri;
      if (!err.empty())
        ap->scan_result.entries.clear();
      else
        ap->scan_result.entries = std::move(v);
      ap->scan_ready_flag.store(true, std::memory_order_release);
    }
  });
}

} // namespace eh::file_browser
