// accounts.cpp — GOA D-Bus + gio network volumes (see accounts.hpp).

#include "app/file_browser/features/accounts/accounts.hpp"

#include "../../app.hpp"

#include "base/thread/thread_dispatch.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <chrono>
#include <thread>

#include <sys/types.h>
#include <unistd.h>

namespace eh::file_browser {

namespace {

constexpr const char* kGoaBus = "org.gnome.OnlineAccounts";
constexpr const char* kGoaRootPath = "/org/gnome/OnlineAccounts";
constexpr const char* kGoaManagerPath = "/org/gnome/OnlineAccounts/Manager";
constexpr const char* kGoaManagerIface = "org.gnome.OnlineAccounts.Manager";
constexpr const char* kGoaAccountIface = "org.gnome.OnlineAccounts.Account";
constexpr const char* kObjectManagerIface = "org.freedesktop.DBus.ObjectManager";

std::string themed_icon_first(GIcon* icon, const char* fallback) {
  if (G_IS_THEMED_ICON(icon)) {
    const char* const* names = nullptr;
    g_object_get(icon, "names", &names, nullptr);
    if (names && names[0] && *names[0]) return names[0];
  }
  return fallback;
}

} // namespace

bool goa_daemon_available() {
  try {
    auto conn = sdbus::createSessionBusConnection();
    auto proxy = sdbus::createProxy(
        *conn, sdbus::ServiceName{"org.freedesktop.DBus"},
        sdbus::ObjectPath{"/org/freedesktop/DBus"});
    bool owned = false;
    proxy->callMethod("NameHasOwner")
        .onInterface("org.freedesktop.DBus")
        .withArguments(std::string(kGoaBus))
        .storeResultsTo(owned);
    return owned;
  } catch (...) {
    return false;
  }
}

std::vector<GoaAccount> goa_list_accounts() {
  std::vector<GoaAccount> out;
  try {
    auto conn = sdbus::createSessionBusConnection();
    auto proxy = sdbus::createProxy(*conn, sdbus::ServiceName{kGoaBus},
                                    sdbus::ObjectPath{kGoaRootPath});
    std::map<sdbus::ObjectPath,
             std::map<std::string, std::map<std::string, sdbus::Variant>>>
        objects;
    proxy->callMethod("GetManagedObjects")
        .onInterface(kObjectManagerIface)
        .storeResultsTo(objects);
    for (auto& [path, ifaces] : objects) {
      auto it = ifaces.find(kGoaAccountIface);
      if (it == ifaces.end()) continue;
      GoaAccount acc;
      acc.object_path = path;
      auto get = [&](const char* key, std::string& dst) {
        auto f = it->second.find(key);
        if (f == it->second.end()) return;
        try {
          dst = f->second.get<std::string>();
        } catch (...) {
        }
      };
      get("ProviderType", acc.provider_type);
      get("ProviderName", acc.provider_name);
      get("PresentationIdentity", acc.identity);
      if (acc.identity.empty()) get("Identity", acc.identity);
      auto att = it->second.find("AttentionNeeded");
      if (att != it->second.end()) {
        try {
          acc.attention_needed = att->second.get<bool>();
        } catch (...) {
        }
      }
      out.push_back(std::move(acc));
    }
  } catch (...) {
  }
  return out;
}

bool goa_add_account(const std::string& provider_type) {
  try {
    auto conn = sdbus::createSessionBusConnection();
    auto proxy = sdbus::createProxy(*conn, sdbus::ServiceName{kGoaBus},
                                    sdbus::ObjectPath{kGoaManagerPath});
    // AddAccount(provider, identity, presentation_identity, credentials,
    // details). NOTE: this creates a credential-less shell only — the daemon
    // is headless and shows no login UI. Production use must go through
    // goa_open_login_ui(); this stays for tests/probes.
    std::map<std::string, sdbus::Variant> credentials;
    std::map<std::string, std::string> details;
    sdbus::ObjectPath created;
    proxy->callMethod("AddAccount")
        .onInterface(kGoaManagerIface)
        .withArguments(provider_type, std::string(), std::string(),
                       credentials, details)
        .storeResultsTo(created);
    (void)created;
    return true;
  } catch (const sdbus::Error& e) {
    fprintf(stderr, "horizon-files: GOA AddAccount(%s) failed: %s\n",
            provider_type.c_str(), e.what());
    return false;
  }
}

bool goa_remove_account(const std::string& object_path) {
  try {
    auto conn = sdbus::createSessionBusConnection();
    auto proxy = sdbus::createProxy(*conn, sdbus::ServiceName{kGoaBus},
                                    sdbus::ObjectPath{object_path});
    proxy->callMethod("Remove").onInterface(kGoaAccountIface);
    return true;
  } catch (const sdbus::Error& e) {
    fprintf(stderr, "horizon-files: GOA Remove(%s) failed: %s\n",
            object_path.c_str(), e.what());
    return false;
  }
}

std::vector<GioNetVolume> gio_list_network_volumes() {
  std::vector<GioNetVolume> out;
  GVolumeMonitor* mon = g_volume_monitor_get();
  if (!mon) return out;

  auto push_volume = [&](GVolume* volume) {
    char* cls = g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_CLASS);
    bool network = cls && std::string(cls) == "network";
    g_free(cls);
    if (!network) return;
    GioNetVolume v;
    char* uuid = g_volume_get_uuid(volume);
    v.key = std::string("gio:") + (uuid ? uuid : g_volume_get_name(volume));
    g_free(uuid);
    char* name = g_volume_get_name(volume);
    v.name = name ? name : "Network volume";
    g_free(name);
    GIcon* icon = g_volume_get_symbolic_icon(volume);
    v.icon = themed_icon_first(icon, "folder-remote");
    if (icon) g_object_unref(icon);
    GMount* mount = g_volume_get_mount(volume);
    if (mount) {
      v.mounted = true;
      GFile* root = g_mount_get_default_location(mount);
      if (root) {
        char* uri = g_file_get_uri(root);
        if (uri) v.uri = uri;
        g_free(uri);
        g_object_unref(root);
      }
      g_object_unref(mount);
    }
    out.push_back(std::move(v));
  };

  GList* volumes = g_volume_monitor_get_volumes(mon);
  for (GList* l = volumes; l; l = l->next)
    push_volume(static_cast<GVolume*>(l->data));
  g_list_free_full(volumes, g_object_unref);

  // Non-native mounts without a volume (active sftp/davs connections).
  GList* mounts = g_volume_monitor_get_mounts(mon);
  for (GList* l = mounts; l; l = l->next) {
    GMount* mount = static_cast<GMount*>(l->data);
    if (g_mount_is_shadowed(mount)) continue;
    GVolume* volume = g_mount_get_volume(mount);
    if (volume) {
      g_object_unref(volume);
      continue; // covered above
    }
    GFile* root = g_mount_get_default_location(mount);
    if (!root || g_file_is_native(root)) {
      if (root) g_object_unref(root);
      continue;
    }
    GioNetVolume v;
    char* uuid = g_mount_get_uuid(mount);
    char* uri = g_file_get_uri(root);
    v.key = std::string("gio:") + (uuid ? uuid : (uri ? uri : "?"));
    g_free(uuid);
    char* name = g_mount_get_name(mount);
    v.name = name ? name : (uri ? uri : "Network mount");
    g_free(name);
    GIcon* icon = g_mount_get_symbolic_icon(mount);
    v.icon = themed_icon_first(icon, "folder-remote");
    if (icon) g_object_unref(icon);
    v.mounted = true;
    if (uri) v.uri = uri;
    g_free(uri);
    g_object_unref(root);
    out.push_back(std::move(v));
  }
  g_list_free_full(mounts, g_object_unref);
  g_object_unref(mon);
  return out;
}

namespace {

// Resolve a GioNetVolume entry back to a live GVolume/GMount by key.
struct GioTarget {
  GVolume* volume = nullptr; // exactly one of these set
  GMount* mount = nullptr;
};

GioTarget gio_find_target(const std::string& key) {
  GioTarget t;
  GVolumeMonitor* mon = g_volume_monitor_get();
  if (!mon) return t;
  GList* volumes = g_volume_monitor_get_volumes(mon);
  for (GList* l = volumes; l && !t.volume && !t.mount; l = l->next) {
    GVolume* volume = static_cast<GVolume*>(l->data);
    char* uuid = g_volume_get_uuid(volume);
    std::string k = std::string("gio:") + (uuid ? uuid : "");
    g_free(uuid);
    if (k == key) {
      t.volume = static_cast<GVolume*>(g_object_ref(volume));
      break;
    }
    // Fall back to mount-uuid match for mounts listed via volumes.
    GMount* mount = g_volume_get_mount(volume);
    if (mount) {
      char* muuid = g_mount_get_uuid(mount);
      char* uri = nullptr;
      GFile* root = g_mount_get_default_location(mount);
      if (root) {
        uri = g_file_get_uri(root);
        g_object_unref(root);
      }
      std::string mk = std::string("gio:") + (muuid ? muuid : (uri ? uri : "?"));
      g_free(muuid);
      g_free(uri);
      if (mk == key) t.mount = static_cast<GMount*>(g_object_ref(mount));
      g_object_unref(mount);
    }
  }
  g_list_free_full(volumes, g_object_unref);
  if (!t.volume && !t.mount) {
    GList* mounts = g_volume_monitor_get_mounts(mon);
    for (GList* l = mounts; l; l = l->next) {
      GMount* mount = static_cast<GMount*>(l->data);
      char* muuid = g_mount_get_uuid(mount);
      char* uri = nullptr;
      GFile* root = g_mount_get_default_location(mount);
      if (root) {
        uri = g_file_get_uri(root);
        g_object_unref(root);
      }
      std::string mk = std::string("gio:") + (muuid ? muuid : (uri ? uri : "?"));
      g_free(muuid);
      g_free(uri);
      if (mk == key) {
        t.mount = static_cast<GMount*>(g_object_ref(mount));
        break;
      }
    }
    g_list_free_full(mounts, g_object_unref);
  }
  g_object_unref(mon);
  return t;
}

void gio_op_toast(AppState* app, std::string text) {
  DeferredCall::callLater([app, text = std::move(text)]() mutable {
    app->operation_status = std::move(text);
    app->operation_status_expires_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count() +
        3000;
    refresh_sidebar(*app);
    draw(*app);
  });
}

struct MountLoop {
  GMainLoop* loop = nullptr;
  bool done = false;
  bool ok = false;
  std::string err;
};

void on_vol_mounted(GObject* src, GAsyncResult* res, gpointer data) {
  auto* ml = static_cast<MountLoop*>(data);
  GError* err = nullptr;
  ml->ok = !!g_volume_mount_finish(G_VOLUME(src), res, &err);
  if (!ml->ok) ml->err = (err && err->message) ? err->message : "mount failed";
  if (err) g_error_free(err);
  ml->done = true;
  g_main_loop_quit(ml->loop);
}

void on_mnt_unmounted(GObject* src, GAsyncResult* res, gpointer data) {
  auto* ml = static_cast<MountLoop*>(data);
  GError* err = nullptr;
  ml->ok = !!g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &err);
  if (!ml->ok) ml->err = (err && err->message) ? err->message : "unmount failed";
  if (err) g_error_free(err);
  ml->done = true;
  g_main_loop_quit(ml->loop);
}

} // namespace

void gio_mount_volume(AppState& app, const std::string& key) {
  AppState* ap = &app;
  std::thread([ap, key]() {
    GioTarget t = gio_find_target(key);
    std::string navigate_uri;
    std::string err;
    do {
      GVolume* vol = t.volume;
      if (!vol) {
        if (t.mount) {
          GFile* root = g_mount_get_default_location(t.mount);
          if (root) {
            char* uri = g_file_get_uri(root);
            if (uri) navigate_uri = uri;
            g_free(uri);
            g_object_unref(root);
          }
          g_object_unref(t.mount);
          break; // already mounted: just navigate
        }
        err = "Volume not found";
        break;
      }
      GMount* existing = g_volume_get_mount(vol);
      if (existing) {
        GFile* root = g_mount_get_default_location(existing);
        if (root) {
          char* uri = g_file_get_uri(root);
          if (uri) navigate_uri = uri;
          g_free(uri);
          g_object_unref(root);
        }
        g_object_unref(existing);
        g_object_unref(vol);
        break;
      }
      GMainContext* ctx = g_main_context_new();
      g_main_context_push_thread_default(ctx);
      GMainLoop* loop = g_main_loop_new(ctx, FALSE);
      MountLoop ml;
      ml.loop = loop;
      // GOA-backed volumes authenticate daemon-side: no explicit operation.
      g_volume_mount(vol, G_MOUNT_MOUNT_NONE, nullptr, nullptr,
                     on_vol_mounted, &ml);
      g_main_loop_run(loop);
      g_main_loop_unref(loop);
      g_main_context_pop_thread_default(ctx);
      g_main_context_unref(ctx);
      if (!ml.ok) {
        err = ml.err;
        g_object_unref(vol);
        break;
      }
      GMount* mounted = g_volume_get_mount(vol);
      if (mounted) {
        GFile* root = g_mount_get_default_location(mounted);
        if (root) {
          char* uri = g_file_get_uri(root);
          if (uri) navigate_uri = uri;
          g_free(uri);
          g_object_unref(root);
        }
        g_object_unref(mounted);
      }
      g_object_unref(vol);
      if (navigate_uri.empty()) err = "Mount gave no location";
    } while (false);

    if (!navigate_uri.empty()) {
      DeferredCall::callLater([ap, navigate_uri]() {
        navigate_to(*ap, navigate_uri);
        draw(*ap);
      });
    } else {
      gio_op_toast(ap, err.empty() ? "Mount failed" : err);
    }
  }).detach();
}

void gio_unmount_volume(AppState& app, const std::string& key) {
  AppState* ap = &app;
  std::thread([ap, key]() {
    GioTarget t = gio_find_target(key);
    GMount* mount = t.mount;
    if (!mount && t.volume) {
      mount = g_volume_get_mount(t.volume);
      g_object_unref(t.volume);
    }
    std::string err;
    bool ok = false;
    if (mount) {
      GMainContext* ctx = g_main_context_new();
      g_main_context_push_thread_default(ctx);
      GMainLoop* loop = g_main_loop_new(ctx, FALSE);
      MountLoop ml;
      ml.loop = loop;
      g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, nullptr,
                                     nullptr, on_mnt_unmounted, &ml);
      g_main_loop_run(loop);
      g_main_loop_unref(loop);
      g_main_context_pop_thread_default(ctx);
      g_main_context_unref(ctx);
      ok = ml.ok;
      err = ml.err;
      g_object_unref(mount);
    } else {
      err = "Nothing to unmount";
    }
    if (ok)
      gio_op_toast(ap, "Unmounted");
    else
      gio_op_toast(ap, err.empty() ? "Unmount failed" : err);
  }).detach();
}

void refresh_settings_accounts(AppState& app) {
  app.settings_goa_available = goa_daemon_available();
  if (app.settings_goa_available)
    app.settings_accounts = goa_list_accounts();
  else
    app.settings_accounts.clear();
  app.settings_google_drive_supported = gio_scheme_supported("google-drive");
  app.settings_davs_supported =
      gio_scheme_supported("davs") || gio_scheme_supported("dav");
  app.settings_accounts_stale = false;
}

bool gio_scheme_supported(const std::string& scheme) {
  const char* const* schemes = g_vfs_get_supported_uri_schemes(g_vfs_get_default());
  if (!schemes) return false;
  for (const char* const* s = schemes; *s; ++s) {
    if (scheme == *s) return true;
  }
  return false;
}

// The GOA daemon is headless: AddAccount alone never shows a login dialog.
// Sign-in UI comes from libgoa-backend's add-dialog, which we host in the
// bundled horizon-goa-signin helper (same dialog Settings uses); when the
// helper isn't installed we fall back to gnome-control-center's panel.
bool goa_open_login_ui(const std::string& provider_type) {
  gchar* helper = g_find_program_in_path("horizon-goa-signin");
  bool use_helper = helper != NULL;
  g_free(helper);
  gchar* settings = use_helper ? NULL : g_find_program_in_path("gnome-control-center");
  if (!use_helper && settings == NULL) return false;
  g_free(settings);
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    if (use_helper) {
      execlp("horizon-goa-signin", "horizon-goa-signin", provider_type.c_str(),
             nullptr);
    } else {
      execlp("gnome-control-center", "gnome-control-center",
             "online-accounts", nullptr);
    }
    _exit(127);
  }
  return true;
}

void goa_remove_account_async(AppState& app, const std::string& object_path) {
  AppState* ap = &app;
  std::thread([ap, object_path]() {
    bool ok = goa_remove_account(object_path);
    DeferredCall::callLater([ap, ok]() {
      ap->settings_accounts_stale = true;
      if (ok) {
        ap->operation_status = "Account removed";
        refresh_settings_accounts(*ap);
      } else {
        ap->operation_status = "Could not remove account";
      }
      ap->operation_status_expires_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count() +
          3000;
      ap->settings_pendingRedraw = true;
      draw(*ap);
    });
  }).detach();
}

} // namespace eh::file_browser
