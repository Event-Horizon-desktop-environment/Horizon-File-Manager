#pragma once

// remote.hpp — GVfs-backed remote browsing (SFTP in slice 1).
//
// Design: listings reuse the existing scan slot (scan_thread /
// scan_generation / scan_result / ready flags), so paint, filter, counts,
// history and back/forward work unchanged. The worker runs GIO on a private
// GMainContext with a single mount call per listing; password retries and
// host-key approvals are collected interactively off-loop (see
// RemoteAuthRequest) and applied back to the held mount operation.

#include <gio/gio.h>

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace eh::file_browser {

class AppState;
struct FileEntry;

// Result of an interactive remote-auth prompt (fulfilled on the UI thread,
// consumed by the listing worker).
struct RemoteAuthResult {
  bool ok = false; // false = user cancelled
  std::string username;
  std::string password;
  int choice = -1; // approval-button index for ask-question prompts
};

// Live interactive-auth request published by the worker. Guarded by
// scan_mtx; `promise` is set exactly once (submit, cancel, or worker abort).
struct RemoteAuthRequest {
  bool active = false;
  uint64_t gen = 0;
  std::string message;
  std::string username; // prefill (login name from the URI)
  bool need_user = false;
  bool need_password = true;
  std::vector<std::string> choices; // non-empty → approval mode (no fields)
  std::shared_ptr<std::promise<RemoteAuthResult>> promise;
};


// True for remote URIs we route to the remote worker (sftp:// in slice 1).
bool is_remote_uri(const std::string& path);

// Normalize for use as a listing identity: drop trailing '/' except root.
std::string remote_normalize(const std::string& uri);

// Parent URI, or "" when already at the scheme root (no "up" available).
std::string remote_parent(const std::string& uri);

// sftp://[user@]host[:port][/path]; port 0/22 omitted, path default "/".
std::string build_sftp_uri(const std::string& host, const std::string& user,
                           int port, const std::string& path);

// True when gvfs can serve the URI scheme (checks supported schemes).
bool remote_scheme_available(const std::string& scheme);


// current_path must be a remote URI. Takes over the scan slot: joins any
// in-flight scan, bumps the generation, spawns the GIO worker. Results
// land in scan_result and are applied by the normal apply path.
void reload_remote_dir(AppState& app);


struct VfsInfo {
  bool exists = false;
  bool is_dir = false;
  uint64_t size = 0;
  int64_t mtime = 0;
};

// stat-like probe for local paths and remote URIs (::stat semantics for
// local files, matching the conflict scanner's expectations).
VfsInfo vfs_stat(const std::string& path);
bool vfs_exists(const std::string& path);
bool vfs_is_dir(const std::string& path);
// Same-file check across both worlds (false for local-vs-remote pairs).
bool vfs_equivalent(const std::string& a, const std::string& b);
// Recursive delete (fs::remove_all locally).
bool vfs_remove_all(const std::string& path);
// Rename within one filesystem (g_file_move remotely).
bool vfs_rename(const std::string& from, const std::string& to);
// Basename rename in place; returns the new path/URI, "" on failure.
std::string vfs_rename_entry(const std::string& path, const std::string& new_name);
bool vfs_mkdir(const std::string& path);
bool vfs_create_empty(const std::string& path);
bool vfs_symlink(const std::string& target, const std::string& link_path);

// Async copy/move where any src or dest is remote. Mirrors start_async_op's
// contract (progress + completion on the UI thread via DeferredCall).
// Undo records are the caller's job — remote ops skip them (slice 1).
// error_out (optional) receives a human-readable failure summary.
void remote_do_copy_move(std::vector<std::string> src_paths,
                         std::string dest_dir, bool is_move,
                         std::shared_ptr<struct OperationProgress> prog,
                         std::function<void(bool cancelled)> on_complete,
                         std::vector<std::string> allow_overwrite,
                         std::vector<std::string> dst_names,
                         std::shared_ptr<std::string> error_out = nullptr);

// Abort any in-flight remote GIO operation promptly (called before
// join_scan() from both reload_dir and reload_remote_dir).
void remote_cancel_inflight(AppState& app);

// Map one enumerated child to a FileEntry (pure except GFileInfo access).
// Exposed for headless tests with synthetic GFileInfo objects.
FileEntry remote_entry_from_info(GFileInfo* info, const std::string& child_uri);

} // namespace eh::file_browser
