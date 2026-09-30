#pragma once

// accounts.hpp — Online Accounts (GOA) + network volume listing.
//
// Slice 1: enumerate GOA accounts over the session bus (graceful when the
// daemon is absent), add/remove accounts, and list network gio volumes
// (goa-backed Drive/Nextcloud mounts, bare sftp mounts) for the sidebar.
// Mount/unmount run on worker threads; UI refresh goes through DeferredCall.

#include <functional>
#include <string>
#include <vector>

namespace eh::file_browser {

class AppState;

struct GoaAccount {
  std::string object_path;
  std::string provider_type; // "google", "owncloud", ...
  std::string provider_name; // "Google", "Nextcloud", ...
  std::string identity;      // user-visible account id
  bool attention_needed = false;
};

// True when org.gnome.OnlineAccounts answers on the session bus. Never
// throws; false also covers headless/test sessions without a bus.
bool goa_daemon_available();

// All accounts currently known to GOA (empty when unavailable).
std::vector<GoaAccount> goa_list_accounts();

// Create an empty GOA account shell (D-Bus AddAccount). Shows NO login UI —
// the daemon is headless; the sign-in dialog lives in gnome-control-center.
// Kept for tests/probes; the UI must use goa_open_login_ui() instead.
bool goa_add_account(const std::string& provider_type);

// Remove an account by object path. Returns success.
bool goa_remove_account(const std::string& object_path);

// Launch the real GNOME sign-in UI for a provider ("google", "owncloud").
// Prefers the bundled horizon-goa-signin helper (the genuine libgoa-backend
// add-dialog: "Sign in with your browser" + system browser + OAuth
// redirect), falling back to gnome-control-center's Online Accounts panel.
// Returns false when neither is installed — the caller should toast
// guidance instead. Raw AddAccount is deliberately NOT used here: the
// daemon is headless and it would only strand a credential-less shell.
bool goa_open_login_ui(const std::string& provider_type);

// Fire-and-forget remove for UI threads: the D-Bus round trip runs on a
// worker thread; toasts and cache refreshes come back through DeferredCall.
void goa_remove_account_async(AppState& app, const std::string& object_path);

struct GioNetVolume {
  std::string key;  // stable id: "gio:<uuid-or-uri>"
  std::string name; // display name
  std::string uri;  // mount/default location, "" when unmounted
  std::string icon; // theme icon name ("folder-remote" fallback)
  bool mounted = false;
};

// Network-class volumes + non-native mounts without volumes (e.g. active
// sftp connections). Cheap local IPC; safe on the UI thread.
std::vector<GioNetVolume> gio_list_network_volumes();

// Mount a network volume by key (worker thread, then refresh + toast).
void gio_mount_volume(AppState& app, const std::string& key);
// Unmount by key (worker thread, then refresh + toast).
void gio_unmount_volume(AppState& app, const std::string& key);

// True when the local gvfs build can browse a URI scheme (e.g.
// "google-drive", "davs"). Used to keep the Accounts tab honest on systems
// whose gvfs ships without those backends.
bool gio_scheme_supported(const std::string& scheme);

// Refresh the Accounts-tab GOA cache (clears the stale flag). Cheap when
// the daemon is absent.
void refresh_settings_accounts(AppState& app);

} // namespace eh::file_browser
