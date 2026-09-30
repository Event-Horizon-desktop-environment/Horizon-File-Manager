// horizon-vfs — single-shot virtual-filesystem helper.
//
// Usage: horizon-vfs list <uri> [--password-stdin]
// Lists a remote (sftp://…) folder via GIO/gvfs WITHOUT interaction:
//   stdout: records "name\tpath\tis_dir\tis_symlink\tsize\tmtime\tmime\t
//           hidden\tread\twrite\tlink_target\textension\0" (NUL-separated,
//           folders-first + case-insensitive name order, like the main app).
//   stderr: "VFS-ERROR <msg>" (exit 1, hard failure) or
//           "VFS-AUTH-REQUIRED <password|question> <message>" (exit 3 — the
//           caller falls back to the interactive in-process mount flow).
// Exit 0 on success (possibly with zero records).
//
// This is the split half of reload_remote_dir() (see
// src/app/file_browser/features/remote/remote.cpp): GFile/GFileInfo,
// GMainLoop-per-listing and the mount state machine live here so libgio
// pressure + mount threads stay outside horizon-files RSS. The main process
// keeps its interactive auth dialogs and uses this helper first, falling
// back in-process when auth is needed or the helper is missing.
#include <gio/gio.h>
#include <glib.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kChildAttrs =
    "standard::name,standard::display-name,standard::type,"
    "standard::is-symlink,standard::symlink-target,standard::size,"
    "time::modified,time::modified-usec,standard::content-type,"
    "standard::fast-content-type,access::can-read,access::can-write";

struct Entry {
  std::string name;
  std::string path;
  bool is_dir = false;
  bool is_symlink = false;
  uint64_t size = 0;
  int64_t mtime = 0;
  std::string mime;
  bool hidden = false;
  bool readable = true;
  bool writable = true;
  std::string link_target;
  std::string ext;
};

std::string lower_ext(const std::string& name) {
  auto dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return {};
  std::string ext = name.substr(dot + 1);
  for (auto& c : ext)
    c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

void emit_entry(const Entry& e) {
  char meta[128];
  int n = std::snprintf(meta, sizeof(meta), "%d\t%d\t%llu\t%lld\t",
                        e.is_dir ? 1 : 0, e.is_symlink ? 1 : 0,
                        (unsigned long long)e.size, (long long)e.mtime);
  std::fwrite(e.name.c_str(), 1, e.name.size(), stdout);
  std::fputc('\t', stdout);
  std::fwrite(e.path.c_str(), 1, e.path.size(), stdout);
  std::fputc('\t', stdout);
  std::fwrite(meta, 1, static_cast<size_t>(n), stdout);
  std::fwrite(e.mime.c_str(), 1, e.mime.size(), stdout);
  std::fputc('\t', stdout);
  std::fprintf(stdout, "%d\t%d\t%d\t", e.hidden ? 1 : 0,
               e.readable ? 1 : 0, e.writable ? 1 : 0);
  std::fwrite(e.link_target.c_str(), 1, e.link_target.size(), stdout);
  std::fputc('\t', stdout);
  std::fwrite(e.ext.c_str(), 1, e.ext.size(), stdout);
  std::fputc('\0', stdout);
}

struct MountState {
  GMainLoop* loop = nullptr;
  GFile* root = nullptr;
  bool done = false;
  bool ok = false;
  bool want_auth = false;
  bool want_question = false;
  std::string auth_kind;
  std::string auth_msg;
  std::string err;
};

void on_mount_done(GObject*, GAsyncResult* res, gpointer data) {
  auto* st = static_cast<MountState*>(data);
  GError* err = nullptr;
  st->ok = g_file_mount_enclosing_volume_finish(st->root, res, &err);
  if (!st->ok && err) {
    st->err = err->message ? err->message : "mount failed";
    g_error_free(err);
  }
  st->done = true;
  g_main_loop_quit(st->loop);
}

void on_ask_pw(GMountOperation*, char*, char*, char*, char* msg,
               gpointer data) {
  auto* st = static_cast<MountState*>(data);
  st->want_auth = true;
  st->auth_kind = "password";
  st->auth_msg = msg ? msg : "";
  g_main_loop_quit(st->loop);
}

void on_ask_q(GMountOperation*, char* msg, char** choices, gpointer data) {
  auto* st = static_cast<MountState*>(data);
  st->want_auth = true;
  st->want_question = true;
  st->auth_kind = "question";
  st->auth_msg = msg ? msg : "";
  (void)choices;
  g_main_loop_quit(st->loop);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || std::string(argv[1]) != "list") {
    std::fprintf(stderr, "usage: %s list <uri> [--password-stdin]\n", argv[0]);
    return 2;
  }
  std::string uri = argv[2];
  std::string password;
  for (int i = 3; i < argc; ++i)
    if (std::string(argv[i]) == "--password-stdin") {
      char buf[4096];
      if (std::fgets(buf, sizeof(buf), stdin)) {
        password = buf;
        while (!password.empty() &&
               (password.back() == '\n' || password.back() == '\r'))
          password.pop_back();
      }
    }

  GMainContext* ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);
  GMainLoop* loop = g_main_loop_new(ctx, FALSE);
  GCancellable* canc = g_cancellable_new();

  GFile* root = g_file_new_for_uri(uri.c_str());
  GMountOperation* op = g_mount_operation_new();
  g_mount_operation_set_anonymous(op, FALSE);
  if (!password.empty()) {
    g_mount_operation_set_password(op, password.c_str());
    g_mount_operation_set_password_save(op, G_PASSWORD_SAVE_NEVER);
  }
  // Scrub the session password from our own address space ASAP.
  {
    volatile char* p = password.data();
    for (size_t i = 0; i < password.size(); ++i) p[i] = 0;
    password.clear();
  }

  MountState st;
  st.loop = loop;
  st.root = root;
  g_signal_connect(op, "ask-password", G_CALLBACK(on_ask_pw), &st);
  g_signal_connect(op, "ask-question", G_CALLBACK(on_ask_q), &st);
  // Single non-interactive attempt: any auth prompt means "fall back".
  g_file_mount_enclosing_volume(root, G_MOUNT_MOUNT_NONE, op, canc,
                                on_mount_done, &st);
  g_main_loop_run(loop);
  bool mounted = st.done && st.ok && !st.want_auth;
  if (st.want_auth) {
    std::fprintf(stderr, "VFS-AUTH-REQUIRED %s %s\n", st.auth_kind.c_str(),
                 st.auth_msg.c_str());
    g_object_unref(op);
    g_object_unref(root);
    g_object_unref(canc);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return 3;
  }
  if (!mounted) {
    std::fprintf(stderr, "VFS-ERROR %s\n",
                 st.err.empty() ? "mount failed" : st.err.c_str());
    g_object_unref(op);
    g_object_unref(root);
    g_object_unref(canc);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return 1;
  }

  GError* eerr = nullptr;
  GFileEnumerator* en = g_file_enumerate_children(root, kChildAttrs,
                                                  G_FILE_QUERY_INFO_NONE,
                                                  canc, &eerr);
  if (!en) {
    std::fprintf(stderr, "VFS-ERROR %s\n",
                 eerr && eerr->message ? eerr->message : "cannot list folder");
    if (eerr) g_error_free(eerr);
    g_object_unref(op);
    g_object_unref(root);
    g_object_unref(canc);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return 1;
  }

  std::vector<Entry> out;
  for (;;) {
    GError* nerr = nullptr;
    GFileInfo* info = g_file_enumerator_next_file(en, canc, &nerr);
    if (nerr) {
      if (!g_error_matches(nerr, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        std::fprintf(stderr, "VFS-ERROR %s\n",
                     nerr->message ? nerr->message : "listing failed");
      g_error_free(nerr);
      break;
    }
    if (!info) break;
    const char* cn = g_file_info_get_name(info);
    if (!cn || !*cn) {
      g_object_unref(info);
      continue;
    }
    Entry e;
    e.name = cn;
    GFile* child = g_file_get_child(root, cn);
    char* cu = child ? g_file_get_uri(child) : nullptr;
    e.path = cu ? cu : uri;
    if (cu) g_free(cu);
    if (child) g_object_unref(child);
    GFileType t = g_file_info_get_file_type(info);
    e.is_dir = (t == G_FILE_TYPE_DIRECTORY);
    e.is_symlink =
        g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK) &&
        g_file_info_get_is_symlink(info);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
      e.size = e.is_dir ? 0 : g_file_info_get_size(info);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
      GDateTime* dt = g_file_info_get_modification_date_time(info);
      if (dt) {
        e.mtime = g_date_time_to_unix(dt);
        g_date_time_unref(dt);
      }
    }
    const char* ct = g_file_info_get_content_type(info);
    if (!ct)
      ct = g_file_info_get_attribute_string(
          info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    e.mime = ct ? ct : (e.is_dir ? "inode/directory"
                                 : "application/octet-stream");
    e.hidden = !e.name.empty() && e.name[0] == '.';
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
      e.readable = g_file_info_get_attribute_boolean(
          info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
      e.writable = g_file_info_get_attribute_boolean(
          info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
    if (e.is_symlink) {
      const char* tgt = g_file_info_get_symlink_target(info);
      if (tgt) e.link_target = tgt;
    }
    e.ext = lower_ext(e.name);
    out.push_back(std::move(e));
    g_object_unref(info);
  }
  GError* cerr = nullptr;
  g_file_enumerator_close(en, canc, &cerr);
  if (cerr) g_error_free(cerr);
  g_object_unref(en);

  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
    std::string al = a.name, bl = b.name;
    for (auto& c : al)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& c : bl)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (al != bl) return al < bl;
    return a.name < b.name;
  });
  for (auto& e : out) emit_entry(e);
  std::fflush(stdout);

  g_object_unref(op);
  g_object_unref(root);
  g_object_unref(canc);
  g_main_loop_unref(loop);
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  return 0;
}
