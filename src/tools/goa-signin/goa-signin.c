// horizon-goa-signin — tiny GTK4 host for GNOME Online Accounts' genuine
// provider add-dialog (libgoa-backend).
//
// Usage: horizon-goa-signin <provider-type>   (e.g. google, owncloud)
//
// This is the same dialog gnome-control-center hosts: a libadwaita window
// that says "Sign in with your browser" and opens the system browser on
// accounts.google.com. The OAuth redirect comes back through
// goa-oauth2-handler and the account is persisted to goa-daemon — no
// WebKit, no tokens, and no passwords ever touch this process.
#include <adwaita.h>
#include <gtk/gtk.h>

#include <goa/goa.h>
#include <goabackend/goabackend.h>

#include <stdio.h>

static GoaClient* held_client = NULL;
static GoaProvider* held_provider = NULL;
static int exit_status = 1;

static void add_cb(GObject* src, GAsyncResult* res, gpointer user_data) {
  GtkWindow* win = GTK_WINDOW(user_data);
  GError* error = NULL;
  GoaObject* object =
      goa_provider_add_account_finish(GOA_PROVIDER(src), res, &error);
  if (object != NULL) {
    g_print("horizon-goa-signin: account %s added\n",
            g_dbus_object_get_object_path(G_DBUS_OBJECT(object)));
    g_object_unref(object);
    exit_status = 0;
  } else {
    g_printerr("horizon-goa-signin: %s\n",
               error != NULL ? error->message : "dismissed");
    g_clear_error(&error);
    exit_status = 1;
  }
  g_clear_object(&held_client);
  g_clear_object(&held_provider);
  gtk_window_destroy(win);
}

static void activate_cb(GtkApplication* app, gpointer user_data) {
  const char* provider_type = (const char*)user_data;
  GError* error = NULL;

  held_client = goa_client_new_sync(NULL, &error);
  if (held_client == NULL) {
    g_printerr("horizon-goa-signin: no Online Accounts service: %s\n",
               error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    exit_status = 2;
    g_application_quit(G_APPLICATION(app));
    return;
  }

  held_provider = goa_provider_get_for_provider_type(provider_type);
  if (held_provider == NULL) {
    g_printerr("horizon-goa-signin: unsupported provider '%s'\n",
               provider_type);
    exit_status = 2;
    g_application_quit(G_APPLICATION(app));
    return;
  }

  GtkWidget* win = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(win), "Online Accounts sign-in");
  gtk_window_set_default_size(GTK_WINDOW(win), 480, 360);
  gtk_window_present(GTK_WINDOW(win));
  goa_provider_add_account(held_provider, held_client, win, NULL, add_cb, win);
}

int main(int argc, char** argv) {
  if (argc != 2 || argv[1][0] == '-') {
    g_printerr(
        "usage: horizon-goa-signin <provider-type>  (e.g. google, owncloud)\n");
    return 2;
  }
  adw_init();
  GtkApplication* app = gtk_application_new("org.horizon.GoaSignin",
                                            G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate_cb), argv[1]);
  int status = g_application_run(G_APPLICATION(app), 0, NULL);
  g_object_unref(app);
  return status == 0 ? exit_status : status;
}
