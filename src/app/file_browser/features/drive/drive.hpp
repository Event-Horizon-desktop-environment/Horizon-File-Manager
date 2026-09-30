#pragma once

// drive.hpp — native Google Drive backend (own OAuth2 + Drive v3 API).
//
// Why native: distro gvfs builds (this one included) ship no google-drive
// mount backend, and GOA's Google scopes don't include Drive — so neither
// gio mounts nor GOA tokens can ever show Drive. This module does OAuth2
// (PKCE, loopback redirect, system browser) with the user's own Desktop
// client ID plus Drive v3 listings, feeding the same scan slot as every
// other listing so paint/filter/history work unchanged.
//
// Multi-account: one app-level client ID, N signed-in accounts (email-keyed
// URIs: googledrive://user@gmail.com/path). Tokens are session-only and
// never touch disk; only the client ID is persisted.
//
// Slice 1 is read-only: browse + open (download-to-temp). Write ops refuse
// honestly via drive_readonly().

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace eh::file_browser {

class AppState;
struct FileEntry;
struct SidebarLocation;
struct OperationProgress;

// One connected Google Drive login. Tokens are session-only and never
// touch disk; only the OAuth client ID is persisted (see drive_client_id).
struct DriveAccount {
  std::string email;
  std::string refresh_token;
  std::string access_token;
  int64_t access_expiry_ms = 0; // steady-clock ms; 0 = fetch now
  std::map<std::string, std::string> id_cache; // display-path → file id
  // The OAuth client used for this login (memory only; the secret never
  // touches disk — it lives in the login keyring when remembered).
  std::string client_id;
  std::string client_secret;
};


// True for native Drive URIs: googledrive://email[/path...].
bool is_drive_uri(const std::string& path);

// Normalize: drop trailing '/' except the account root (keeps one slash).
std::string drive_normalize(const std::string& uri);

// Parent URI, or "" at the account root (no "up" available).
std::string drive_parent(const std::string& uri);

// Authority (account email) and the escaped display path ("/a/b", "" root).
std::string drive_email(const std::string& uri);
std::string drive_display_path(const std::string& uri);

// Build from parts; each display segment is %-escaped so '/' in Drive
// names can never corrupt the path structure.
std::string build_drive_uri(const std::string& email,
                            const std::string& display_path);

// Read-only gate for op entry points (slice 1: browse + open only).
bool drive_readonly(const std::string& path);

// UI thread only: toast "read-only" and return true when path is a Drive
// URI, so write-op entries (delete/rename/copy/move/mkdir) refuse honestly.
bool drive_block_write(AppState& app, const std::string& path);


// base64url (no padding) of raw bytes.
std::string drive_base64url(const uint8_t* data, size_t len);
// S256 PKCE challenge for a verifier string.
std::string drive_code_challenge(const std::string& verifier);
// Random verifier (64 chars, [A-Za-z0-9]).
std::string drive_random_verifier();
// Random state token (hex).
std::string drive_random_state();
// Full authorization URL for the system browser.
std::string drive_auth_url(const std::string& client_id,
                           const std::string& redirect_uri,
                           const std::string& state,
                           const std::string& challenge);


struct DriveToken {
  std::string access_token;
  std::string refresh_token; // empty when the server didn't re-issue one
  int64_t expires_in_sec = 0;
};

// Exchange an auth code for tokens (PKCE). client_secret may be empty
// (Desktop PKCE-only); when present it is sent too — some clients demand
// it even with a code_verifier. False + err on failure.
bool drive_exchange_code(const std::string& client_id,
                         const std::string& client_secret,
                         const std::string& code, const std::string& verifier,
                         const std::string& redirect_uri, DriveToken& out,
                         std::string& err);

// Refresh an access token (secret forwarded when present). False + err.
bool drive_refresh_token(const std::string& client_id,
                         const std::string& client_secret,
                         const std::string& refresh_token,
                         std::string& access_out, int64_t& expires_in_out,
                         std::string& err);

// Email for an access token via userinfo. False + err on failure.
bool drive_fetch_email(const std::string& access_token, std::string& email_out,
                       std::string& err);

struct DriveItem {
  std::string id;
  std::string name;
  std::string mime;
  bool is_folder = false;
  uint64_t size = 0;
  int64_t mtime_sec = 0;
};

// Map one Drive item to a FileEntry (pure; unit-testable headless).
// dparent_raw is the parent's raw (escaped) display path ("" at root).
FileEntry drive_entry_from_item(const std::string& email,
                                const std::string& dparent_raw,
                                const DriveItem& item);

// List a folder's children (all pages). False + err on failure. `gen_done`
// is polled between pages: return true from it to abort quietly.
bool drive_list_children(const std::string& access_token,
                         const std::string& folder_id,
                         std::vector<DriveItem>& out, std::string& err,
                         const std::function<bool()>& gen_done,
                         const std::string& drive_id = "");

// Download a file's bytes. False + err on failure.
bool drive_download(const std::string& access_token,
                    const std::string& file_id, std::vector<uint8_t>& out,
                    std::string& err, const std::string& drive_id = "");

// Export a Google-native file (Docs/Sheets/…) to a concrete format.
// False + err on failure.
bool drive_export(const std::string& access_token, const std::string& file_id,
                  const std::string& export_mime, std::vector<uint8_t>& out,
                  std::string& err, const std::string& drive_id = "");


// True when built with libsecret support.
bool drive_keyring_available();

// Store one account's secrets (single blob keyed by email). UI thread.
void drive_keyring_store(const std::string& email,
                         const std::string& client_id,
                         const std::string& client_secret,
                         const std::string& refresh_token);
// Look up secrets. Any thread. False when absent/unavailable.
bool drive_keyring_lookup(const std::string& email, std::string& client_id_out,
                          std::string& secret_out, std::string& refresh_out);
// Forget. UI thread.
void drive_keyring_erase(const std::string& email);

// Startup restore (UI thread): re-auth remembered accounts in the
// background; sidebar refreshes itself when they land.
void drive_restore_accounts(AppState& app);


struct DriveStat {
  bool exists = false;
  bool is_dir = false;
  uint64_t size = 0;
  int64_t mtime = 0;
};

// Bind the process AppState for AppState-free vfs_* drive branches.
// Called once at startup (the app is single-AppState).
void drive_bind_app(AppState& app);

// stat / mkdir / create-empty / trash / permanent-delete / same-account
// move+rename by URI. All return false + err on failure.
bool drive_stat_uri(const std::string& uri, DriveStat& out, std::string& err);
bool drive_mkdir_path(const std::string& uri, std::string& err);
bool drive_create_empty_path(const std::string& uri, const std::string& mime,
                             std::string& err);
bool drive_trash_path(const std::string& uri, std::string& err);
bool drive_delete_path(const std::string& uri, std::string& err);
bool drive_move_path(const std::string& from_uri, const std::string& to_uri,
                     std::string& err);

// Resumable upload / streaming download with progress (nullable prog).
// cancel is polled between chunks; empty err on cancel.
bool drive_upload_file(const std::string& access, const std::string& parent_id,
                       const std::string& name, const std::string& mime,
                       const std::string& local_path, OperationProgress* prog,
                       const std::function<bool()>& cancel,
                       std::string& id_out, std::string& err,
                       const std::string& drive_id = "");
bool drive_download_to(const std::string& access, const std::string& file_id,
                       const std::string& export_mime,
                       const std::string& local_path, OperationProgress* prog,
                       const std::function<bool()>& cancel, std::string& err,
                       const std::string& drive_id = "");

// Async copy/move where any src or dest is a Drive URI. Mirrors
// remote_do_copy_move's contract (progress + completion on the UI thread
// via DeferredCall). No undo records, like remote ops.
void drive_do_copy_move(std::vector<std::string> src_paths,
                        std::string dest_dir, bool is_move,
                        std::shared_ptr<struct OperationProgress> prog,
                        std::function<void(bool cancelled)> on_complete,
                        std::vector<std::string> allow_overwrite,
                        std::vector<std::string> dst_names,
                        std::shared_ptr<std::string> error_out = nullptr);


struct SharedDrive {
  std::string id;
  std::string name;
};

// All shared drives visible to the token. False + err on failure.
bool drive_list_shared_drives(const std::string& access_token,
                              std::vector<SharedDrive>& out, std::string& err);

// Location with corpus context (drive_id "" = My Drive). Pseudo-root
// "/Shared drives" resolves to id "shared:roots".
struct DriveLoc {
  std::string id;
  std::string drive_id;
};
DriveLoc drive_locate(AppState& app, const std::string& email,
                      const std::string& display_path, std::string& err,
                      const std::function<bool()>& gen_done);

// Strip a "shared:<id>" marker to the real id for API calls.
inline std::string drive_real_id(const std::string& id) {
  constexpr const char* kMark = "shared:";
  if (id.rfind(kMark, 0) == 0)
    return id.substr(std::char_traits<char>::length(kMark));
  return id;
}


struct DriveMeta {
  bool exists = false;
  bool is_dir = false;
  uint64_t size = 0;
  int64_t mtime = 0;
  int64_t ctime = 0;   // createdTime
  int64_t viewed = 0;  // viewedByMeTime
  std::string mime;
  std::string owner;       // display name
  std::string owner_email;
  bool shared = false;
  std::string version;
  std::string md5;
};

// Full metadata for one file id. False + err on failure.
bool drive_file_meta(const std::string& access_token, const std::string& id,
                     const std::string& drive_id, DriveMeta& out,
                     std::string& err);

// "My Drive <used> of <total>" backing the Properties volume donut.
// total == 0 when the account reports no limit (donut hides itself).
bool drive_about_quota(const std::string& access_token, uint64_t& used_out,
                       uint64_t& total_out, std::string& err);

struct DriveTreeSize {
  uint64_t files = 0;
  uint64_t dirs = 0;
  uint64_t bytes = 0;
};

// Recursive file/dir/byte totals under a folder id. Worker threads.
bool drive_walk_size(const std::string& access_token,
                     const std::string& folder_id, const std::string& drive_id,
                     DriveTreeSize& out, std::string& err,
                     const std::function<bool()>& gen_done);

// Export target for a Google-native mime ("" when not exportable).
// Suffix includes the leading dot; empty when not exportable.
std::string drive_export_target(const std::string& mime,
                                std::string& suffix_out);

// Recursive file/dir/byte totals under a Drive folder URI (bound app +
// network). Worker threads. False + err on failure.
bool drive_folder_size(const std::string& uri, uint64_t& files_out,
                       uint64_t& dirs_out, uint64_t& bytes_out,
                       std::string& err);

// Friendly kind label for Google-native mimes ("Google Docs", …).
// Returns nullptr for non-Google mimes (use the regular pipeline).
const char* drive_kind_label(const std::string& mime);


// Resolve a display path to a Drive file id, walking + caching per
// account (worker-thread safe via scan_mtx; caller must hold no locks).
// Returns "" + err on failure.
std::string drive_resolve_id(AppState& app, const std::string& email,
                             const std::string& display_path,
                             std::string& err,
                             const std::function<bool()>& gen_done);

// Valid (unexpired) access token for an account, refreshing in place.
// Worker threads only. False + err on failure.
bool drive_ensure_token(AppState& app, const std::string& email,
                        std::string& access_out, std::string& err);

// Listing worker: takes over the scan slot like reload_remote_dir.
void reload_drive_dir(AppState& app);

// Interactive connect: opens the system browser, captures the loopback
// redirect, exchanges tokens, appends the account. Worker thread; results
// (toast + sidebar + navigate) come back through DeferredCall. The secret
// stays in memory only and is never persisted anywhere.
void drive_connect_account(AppState& app, std::string client_id,
                           std::string client_secret);

// Forget an account's tokens (sidebar refreshes; no disk ever held them).
void drive_disconnect(AppState& app, const std::string& email);

// Sidebar rows for connected accounts (called from refresh_sidebar).
std::vector<struct SidebarLocation> drive_sidebar_rows(const AppState& app);

// Open a Drive file: download to temp, launch default app (worker + UI).
void drive_open_file(AppState& app, const FileEntry& entry);

// Find an account by email; nullptr if absent.
const DriveAccount* drive_find(const AppState& app, const std::string& email);
DriveAccount* drive_find(AppState& app, const std::string& email);

} // namespace eh::file_browser
