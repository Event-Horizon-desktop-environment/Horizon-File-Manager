// drive.cpp — native Google Drive backend (see drive.hpp).

#include "app/file_browser/features/drive/drive.hpp"

#include "../../app.hpp"

#include "app/file_browser/features/filetype/filetype.hpp"
#include "base/thread/thread_dispatch.hpp"
#include "config/shell_config.hpp"

#include <gio/gio.h>

#include <openssl/evp.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef EH_HAVE_DRIVE
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#endif

namespace eh::file_browser {

namespace fs = std::filesystem;

namespace {

constexpr const char* kScheme = "googledrive";
constexpr const char* kFolderMime = "application/vnd.google-apps.folder";
constexpr const char* kShortcutMime = "application/vnd.google-apps.shortcut";
constexpr const char* kDriveScope =
    "https://www.googleapis.com/auth/drive openid email";
constexpr const char* kAuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
constexpr const char* kTokenEndpoint = "https://oauth2.googleapis.com/token";
constexpr const char* kUserinfoEndpoint =
    "https://www.googleapis.com/oauth2/v2/userinfo";
constexpr const char* kFilesEndpoint = "https://www.googleapis.com/drive/v3/files";

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void drive_toast(AppState& app, const std::string& msg) {
  app.operation_status = msg;
  app.operation_status_expires_ms = now_ms() + 3000;
  app.pendingRedraw = true;
  draw(app);
}

std::string escape_seg(const std::string& seg) {
  // Escape everything reserved (including '/') so Drive names with slashes
  // can never corrupt the URI path structure. UTF-8 stays readable.
  gchar* e = g_uri_escape_string(seg.c_str(), "", TRUE);
  std::string out = e ? e : seg;
  g_free(e);
  return out;
}

std::string unescape_seg(const std::string& seg) {
  gchar* u = g_uri_unescape_string(seg.c_str(), nullptr);
  std::string out = u ? u : seg;
  g_free(u);
  return out;
}

std::vector<std::string> split_raw(const std::string& display_path) {  // display_path is the raw (escaped) "/a/b" suffix; "" or "/" → none.
  std::vector<std::string> segs;
  size_t i = 0;
  while (i < display_path.size() && display_path[i] == '/') ++i;
  std::string cur;
  for (; i <= display_path.size(); ++i) {
    if (i == display_path.size() || display_path[i] == '/') {
      if (!cur.empty()) {
        segs.push_back(cur);
        cur.clear();
      }
    } else {
      cur += display_path[i];
    }
  }
  return segs;
}

} // namespace

// These have no file extension and can't be downloaded directly; they
// need files.export. ext_hint drives icon classification, export_mime
// (+suffix) drives Open. nullptr export = list-only with an honest toast.

namespace {

struct GoogleKind {
  const char* tail; // after "application/vnd.google-apps."
  const char* ext_hint; // icon classification ("" = name-based/generic)
  const char* export_mime; // nullptr = not exportable via files.export
  const char* export_suffix;
  // Try a plain alt=media download (binary-ish types: audio/video/photos,
  // plain Drive files). Editor-only types stay list-only with a toast.
  bool downloadable;
};

constexpr GoogleKind kGoogleKinds[] = {
    {"document", "docx",
     "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
     ".docx", false},
    {"spreadsheet", "xlsx",
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
     ".xlsx", false},
    {"presentation", "pptx",
     "application/vnd.openxmlformats-officedocument.presentationml.presentation",
     ".pptx", false},
    {"drawing", "png", "image/png", ".png", false},
    {"script", "js", "application/vnd.google-apps.script+json", ".json",
     false},
    // Binary-ish: try a straight download.
    {"audio", "mp3", nullptr, nullptr, true},
    {"video", "mp4", nullptr, nullptr, true},
    {"vid", "mp4", nullptr, nullptr, true},
    {"photo", "jpg", nullptr, nullptr, true},
    {"pic", "jpg", nullptr, nullptr, true},
    {"file", "", nullptr, nullptr, true},
    {"unknown", "", nullptr, nullptr, true},
    // Editor-only: listed with a sensible icon, but not openable here.
    {"form", "pdf", nullptr, nullptr, false},
    {"site", "html", nullptr, nullptr, false},
    {"map", "", nullptr, nullptr, false},
    {"jam", "pdf", nullptr, nullptr, false},
    {"mail-layout", "html", nullptr, nullptr, false},
    {"fusiontable", "csv", nullptr, nullptr, false},
    {"drive-sdk", "", nullptr, nullptr, false},
};

constexpr const char* kGooglePrefix = "application/vnd.google-apps.";

const GoogleKind* google_kind(const std::string& mime) {
  if (mime.rfind(kGooglePrefix, 0) != 0) return nullptr;
  std::string tail = mime.substr(std::char_traits<char>::length(kGooglePrefix));
  for (auto& g : kGoogleKinds)
    if (tail == g.tail) return &g;
  return nullptr;
}

} // namespace


bool is_drive_uri(const std::string& path) {
  if (path.rfind("googledrive://", 0) != 0) return false;
  // Must have a non-empty authority (account email).
  std::string rest = path.substr(std::string("googledrive://").size());
  if (!rest.empty() && rest[0] == '/') return false;
  size_t slash = rest.find('/');
  std::string auth = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  return !auth.empty();
}

std::string drive_normalize(const std::string& uri) {
  auto pos = uri.find("://");
  if (pos == std::string::npos) return uri;
  std::string head = uri.substr(0, pos + 3);
  std::string rest = uri.substr(pos + 3);
  while (rest.size() > 1 && rest.back() == '/') rest.pop_back();
  if (rest.empty() || rest.find('/') == std::string::npos) rest += '/';
  return head + rest;
}

std::string drive_parent(const std::string& uri) {
  std::string n = drive_normalize(uri);
  auto pos = n.find("://");
  if (pos == std::string::npos) return {};
  std::string rest = n.substr(pos + 3);
  if (!rest.empty() && rest.back() == '/') rest.pop_back();
  auto slash = rest.rfind('/');
  if (slash == std::string::npos) return {}; // already at account root
  std::string up = rest.substr(0, slash);
  if (up.find('/') == std::string::npos) up += '/';
  return n.substr(0, pos + 3) + up;
}

std::string drive_email(const std::string& uri) {
  auto pos = uri.find("://");
  if (pos == std::string::npos) return {};
  std::string rest = uri.substr(pos + 3);
  auto slash = rest.find('/');
  return (slash == std::string::npos) ? rest : rest.substr(0, slash);
}

std::string drive_display_path(const std::string& uri) {
  auto pos = uri.find("://");
  if (pos == std::string::npos) return {};
  std::string rest = uri.substr(pos + 3);
  auto slash = rest.find('/');
  if (slash == std::string::npos) return {};
  std::string p = rest.substr(slash);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p == "/" ? std::string{} : p;
}

std::string build_drive_uri(const std::string& email,
                            const std::string& display_path) {
  std::string uri = std::string("googledrive://") + email;
  if (display_path.empty() || display_path == "/") return uri + "/";
  std::string out = uri;
  for (auto& seg : split_raw(display_path)) out += "/" + escape_seg(unescape_seg(seg));
  return out;
}

bool drive_readonly(const std::string& path) { return is_drive_uri(path); }

bool drive_block_write(AppState& app, const std::string& path) {
  if (!is_drive_uri(path)) return false;
  drive_toast(app, "Google Drive is read-only in this build");
  return true;
}


std::string drive_base64url(const uint8_t* data, size_t len) {
  gchar* b64 = g_base64_encode(data, static_cast<guint>(len));
  std::string out = b64 ? b64 : "";
  g_free(b64);
  for (auto& c : out) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!out.empty() && out.back() == '=') out.pop_back();
  return out;
}

std::string drive_code_challenge(const std::string& verifier) {
  uint8_t digest[32];
  unsigned int dlen = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx) {
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, verifier.data(), verifier.size()) == 1)
      EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);
  }
  if (dlen != 32) return {};
  return drive_base64url(digest, 32);
}

std::string drive_random_verifier() {
  static constexpr char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dist(0, sizeof(kChars) - 2);
  std::string out;
  out.reserve(64);
  for (int i = 0; i < 64; ++i) out += kChars[dist(gen)];
  return out;
}

std::string drive_random_state() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<unsigned> dist(0, 255);
  char buf[33];
  for (int i = 0; i < 16; ++i) snprintf(buf + i * 2, 3, "%02x", dist(gen));
  return std::string(buf, 32);
}

std::string drive_auth_url(const std::string& client_id,
                           const std::string& redirect_uri,
                           const std::string& state,
                           const std::string& challenge) {
  auto esc = [](const std::string& s) {
    gchar* e = g_uri_escape_string(s.c_str(), "", TRUE);
    std::string out = e ? e : s;
    g_free(e);
    return out;
  };
  std::string url = kAuthEndpoint;
  url += "?response_type=code&access_type=offline&prompt=consent";
  url += "&client_id=" + esc(client_id);
  url += "&redirect_uri=" + esc(redirect_uri);
  url += "&scope=" + esc(kDriveScope);
  url += "&state=" + esc(state);
  url += "&code_challenge=" + esc(challenge);
  url += "&code_challenge_method=S256";
  return url;
}


#ifdef EH_HAVE_DRIVE

namespace {

std::string soup_bearer(const std::string& access) {
  return "Bearer " + access;
}

// Synchronous GET/POST in a worker thread. Returns body; sets http_status.
std::string soup_fetch(const std::string& url, const std::string& bearer,
                       const std::string& post_body, int& http_status,
                       std::string& err) {
  http_status = 0;
  SoupSession* session = soup_session_new();
  SoupMessage* msg = post_body.empty()
                         ? soup_message_new(SOUP_METHOD_GET, url.c_str())
                         : soup_message_new(SOUP_METHOD_POST, url.c_str());
  if (!msg) {
    err = "cannot build request";
    g_object_unref(session);
    return {};
  }
  if (!bearer.empty())
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Authorization", soup_bearer(bearer).c_str());
  if (!post_body.empty()) {
    GBytes* body = g_bytes_new(post_body.data(), post_body.size());
    soup_message_set_request_body_from_bytes(
        msg, "application/x-www-form-urlencoded", body);
    g_bytes_unref(body);
  }
  GError* gerr = nullptr;
  GBytes* bytes = soup_session_send_and_read(session, msg, nullptr, &gerr);
  http_status = soup_message_get_status(msg);
  std::string out;
  if (bytes) {
    gsize n = 0;
    const auto* data =
        static_cast<const char*>(g_bytes_get_data(bytes, &n));
    if (data && n) out.assign(data, n);
    g_bytes_unref(bytes);
  }
  // Log failures with the endpoint + status + body snippet: Google's
  // error_description is the only way to tell redirect_uri_mismatch from
  // invalid_grant from a bad scope. Terminal/journal only, no tokens.
  if (gerr || http_status < 200 || http_status >= 300) {
    const char* host = strchr(url.c_str(), ':');
    host = host ? strchr(host + 3, '/') : nullptr;
    std::string endpoint = host ? host : url;
    auto qm = endpoint.find('?');
    if (qm != std::string::npos) endpoint.resize(qm);
    fprintf(stderr, "horizon-files: drive %s -> HTTP %d%s%s\n",
            endpoint.c_str(), http_status,
            gerr && gerr->message ? " " : "",
            gerr && gerr->message ? gerr->message : "");
    if (!out.empty())
      fprintf(stderr, "horizon-files: drive body: %.300s\n", out.c_str());
  }
  if (gerr) {
    err = gerr->message ? gerr->message : "request failed";
    g_error_free(gerr);
  } else if (http_status < 200 || http_status >= 300) {
    err = "HTTP " + std::to_string(http_status);
  }
  g_object_unref(msg);
  g_object_unref(session);
  return out;
}

std::string form_escape(const std::string& s) {
  gchar* e = g_uri_escape_string(s.c_str(), "", TRUE);
  std::string out = e ? e : s;
  g_free(e);
  return out;
}

// Append shared-drive params (? or & form) when drive_id is set.
void drive_sup(std::string& url, const std::string& drive_id, bool is_list) {
  if (drive_id.empty()) return;
  url += (url.find('?') == std::string::npos) ? "?" : "&";
  if (is_list)
    url += "corpora=drive&driveId=" + drive_id +
           "&includeItemsFromAllDrives=true&";
  url += "supportsAllDrives=true";
}

bool parse_token_json(const std::string& body, DriveToken& out,
                      std::string& err) {
  JsonParser* parser = json_parser_new();
  GError* gerr = nullptr;
  if (!json_parser_load_from_data(parser, body.c_str(),
                                  static_cast<gssize>(body.size()), &gerr)) {
    err = gerr && gerr->message ? gerr->message : "bad token response";
    if (gerr) g_error_free(gerr);
    g_object_unref(parser);
    return false;
  }
  JsonNode* root = json_parser_get_root(parser);
  JsonObject* obj =
      (root && JSON_NODE_HOLDS_OBJECT(root)) ? json_node_get_object(root) : nullptr;
  if (!obj || !json_object_has_member(obj, "access_token")) {
    const char* desc = nullptr;
    if (obj && json_object_has_member(obj, "error_description"))
      desc = json_object_get_string_member(obj, "error_description");
    else if (obj && json_object_has_member(obj, "error"))
      desc = json_object_get_string_member(obj, "error");
    err = desc ? desc : "token endpoint refused";
    g_object_unref(parser);
    return false;
  }
  out.access_token = json_object_get_string_member(obj, "access_token");
  if (json_object_has_member(obj, "refresh_token"))
    out.refresh_token = json_object_get_string_member(obj, "refresh_token");
  if (json_object_has_member(obj, "expires_in"))
    out.expires_in_sec = json_object_get_int_member(obj, "expires_in");
  g_object_unref(parser);
  return !out.access_token.empty();
}

} // namespace

bool drive_exchange_code(const std::string& client_id,
                         const std::string& client_secret,
                         const std::string& code, const std::string& verifier,
                         const std::string& redirect_uri, DriveToken& out,
                         std::string& err) {
  std::string body = "grant_type=authorization_code&code=" + form_escape(code) +
                     "&client_id=" + form_escape(client_id) + "&code_verifier=" +
                     form_escape(verifier) + "&redirect_uri=" +
                     form_escape(redirect_uri);
  if (!client_secret.empty())
    body += "&client_secret=" + form_escape(client_secret);
  int status = 0;
  std::string resp = soup_fetch(kTokenEndpoint, {}, body, status, err);
  if (resp.empty() || (status < 200 || status >= 300)) {
    if (err.empty()) err = "token exchange failed";
    return false;
  }
  return parse_token_json(resp, out, err);
}

bool drive_refresh_token(const std::string& client_id,
                         const std::string& client_secret,
                         const std::string& refresh_token,
                         std::string& access_out, int64_t& expires_in_out,
                         std::string& err) {
  DriveToken tok;
  std::string body = "grant_type=refresh_token&refresh_token=" +
                     form_escape(refresh_token) + "&client_id=" +
                     form_escape(client_id);
  if (!client_secret.empty())
    body += "&client_secret=" + form_escape(client_secret);
  int status = 0;
  std::string resp = soup_fetch(kTokenEndpoint, {}, body, status, err);
  if (resp.empty() || (status < 200 || status >= 300)) {
    if (err.empty()) err = "token refresh failed";
    return false;
  }
  if (!parse_token_json(resp, tok, err)) return false;
  access_out = tok.access_token;
  expires_in_out = tok.expires_in_sec;
  return true;
}

bool drive_fetch_email(const std::string& access_token, std::string& email_out,
                       std::string& err) {
  int status = 0;
  std::string resp = soup_fetch(kUserinfoEndpoint, access_token, {}, status, err);
  if (resp.empty() || (status < 200 || status >= 300)) {
    if (err.empty()) err = "cannot read account email";
    return false;
  }
  JsonParser* parser = json_parser_new();
  GError* gerr = nullptr;
  bool ok = false;
  if (json_parser_load_from_data(parser, resp.c_str(),
                                 static_cast<gssize>(resp.size()), &gerr)) {
    JsonNode* root = json_parser_get_root(parser);
    JsonObject* obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                          ? json_node_get_object(root)
                          : nullptr;
    if (obj && json_object_has_member(obj, "email")) {
      email_out = json_object_get_string_member(obj, "email");
      ok = !email_out.empty();
    } else {
      err = "no email in account response";
    }
  } else {
    err = gerr && gerr->message ? gerr->message : "bad account response";
    if (gerr) g_error_free(gerr);
  }
  g_object_unref(parser);
  return ok;
}

bool drive_list_children(const std::string& access_token,
                         const std::string& folder_id,
                         std::vector<DriveItem>& out, std::string& err,
                         const std::function<bool()>& gen_done,
                         const std::string& drive_id) {
  std::string qid = folder_id;
  // File ids are [A-Za-z0-9_-] but quote-escape defensively.
  std::string esc;
  for (char c : qid) {
    if (c == '\'') esc += "\\'";
    else esc += c;
  }
  gchar* q = g_uri_escape_string(
      ("'" + esc + "' in parents and trashed=false").c_str(), "", TRUE);
  std::string base = std::string(kFilesEndpoint) + "?q=" + (q ? q : "") +
                     "&fields=" +
                     form_escape("nextPageToken,files(id,name,mimeType,size,"
                                 "modifiedTime,shortcutDetails/targetId,"
                                 "shortcutDetails/targetMimeType)") +
                     "&orderBy=folder,name&pageSize=1000";
  g_free(q);
  drive_sup(base, drive_id, true);
  std::string page;
  do {
    if (gen_done && gen_done()) {
      err.clear();
      return false; // superseded: stay silent
    }
    std::string url = base + (page.empty() ? "" : "&pageToken=" + page);
    int status = 0;
    std::string resp = soup_fetch(url, access_token, {}, status, err);
    if (resp.empty() || (status < 200 || status >= 300)) {
      if (status == 401 || status == 403)
        err = "Drive access denied (HTTP " + std::to_string(status) + ")";
      else if (err.empty())
        err = "Drive listing failed";
      return false;
    }
    JsonParser* parser = json_parser_new();
    GError* gerr = nullptr;
    bool ok = false;
    page.clear();
    if (json_parser_load_from_data(parser, resp.c_str(),
                                   static_cast<gssize>(resp.size()), &gerr)) {
      JsonNode* root = json_parser_get_root(parser);
      JsonObject* obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                            ? json_node_get_object(root)
                            : nullptr;
      if (obj) {
        if (json_object_has_member(obj, "nextPageToken"))
          page = json_object_get_string_member(obj, "nextPageToken");
        JsonArray* files = json_object_has_member(obj, "files")
                               ? json_object_get_array_member(obj, "files")
                               : nullptr;
        if (files) {
          for (guint i = 0; i < json_array_get_length(files); ++i) {
            JsonObject* f = json_array_get_object_element(files, i);
            if (!f || !json_object_has_member(f, "id") ||
                !json_object_has_member(f, "name"))
              continue;
            DriveItem item;
            item.id = json_object_get_string_member(f, "id");
            item.name = json_object_get_string_member(f, "name");
            item.mime = json_object_has_member(f, "mimeType")
                            ? json_object_get_string_member(f, "mimeType")
                            : "";
            if (item.mime == kShortcutMime &&
                json_object_has_member(f, "shortcutDetails")) {
              JsonObject* sd =
                  json_object_get_object_member(f, "shortcutDetails");
              if (sd && json_object_has_member(sd, "targetId"))
                item.id = json_object_get_string_member(sd, "targetId");
              if (sd && json_object_has_member(sd, "targetMimeType"))
                item.mime = json_object_get_string_member(sd, "targetMimeType");
            }
            item.is_folder = (item.mime == kFolderMime);
            if (json_object_has_member(f, "size")) {
              const char* sz = json_object_get_string_member(f, "size");
              try {
                item.size = sz ? std::stoull(sz) : 0;
              } catch (...) {
                item.size = 0;
              }
            }
            if (json_object_has_member(f, "modifiedTime")) {
              const char* mt = json_object_get_string_member(f, "modifiedTime");
              GDateTime* dt =
                  mt ? g_date_time_new_from_iso8601(mt, nullptr) : nullptr;
              if (dt) {
                item.mtime_sec = g_date_time_to_unix(dt);
                g_date_time_unref(dt);
              }
            }
            out.push_back(std::move(item));
          }
        }
        ok = true;
      } else {
        err = "bad Drive response";
      }
    } else {
      err = gerr && gerr->message ? gerr->message : "bad Drive response";
      if (gerr) g_error_free(gerr);
    }
    g_object_unref(parser);
    if (!ok) return false;
  } while (!page.empty());
  return true;
}

bool drive_download(const std::string& access_token,
                    const std::string& file_id, std::vector<uint8_t>& out,
                    std::string& err, const std::string& drive_id) {
  SoupSession* session = soup_session_new();
  std::string url =
      std::string(kFilesEndpoint) + "/" + file_id + "?alt=media";
  drive_sup(url, drive_id, false);
  SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
  if (!msg) {
    err = "cannot build request";
    g_object_unref(session);
    return false;
  }
  soup_message_headers_append(soup_message_get_request_headers(msg),
                              "Authorization",
                              soup_bearer(access_token).c_str());
  GError* gerr = nullptr;
  GBytes* bytes = soup_session_send_and_read(session, msg, nullptr, &gerr);
  int status = soup_message_get_status(msg);
  bool ok = false;
  if (bytes && status >= 200 && status < 300) {
    gsize n = 0;
    const auto* data =
        static_cast<const uint8_t*>(g_bytes_get_data(bytes, &n));
    if (data) out.assign(data, data + n);
    ok = true;
  } else if (gerr) {
    err = gerr->message ? gerr->message : "download failed";
    g_error_free(gerr);
  } else {
    err = "download failed (HTTP " + std::to_string(status) + ")";
  }
  if (bytes) g_bytes_unref(bytes);
  g_object_unref(msg);
  g_object_unref(session);
  return ok;
}

bool drive_export(const std::string& access_token,
                  const std::string& file_id, const std::string& export_mime,
                  std::vector<uint8_t>& out, std::string& err,
                  const std::string& drive_id) {
  SoupSession* session = soup_session_new();
  std::string url = std::string(kFilesEndpoint) + "/" + file_id +
                    "/export?mimeType=" + form_escape(export_mime);
  drive_sup(url, drive_id, false);
  SoupMessage* msg = soup_message_new(SOUP_METHOD_GET, url.c_str());
  if (!msg) {
    err = "cannot build request";
    g_object_unref(session);
    return false;
  }
  soup_message_headers_append(soup_message_get_request_headers(msg),
                              "Authorization",
                              soup_bearer(access_token).c_str());
  GError* gerr = nullptr;
  GBytes* bytes = soup_session_send_and_read(session, msg, nullptr, &gerr);
  int status = soup_message_get_status(msg);
  bool ok = false;
  if (bytes && status >= 200 && status < 300) {
    gsize n = 0;
    const auto* data =
        static_cast<const uint8_t*>(g_bytes_get_data(bytes, &n));
    if (data) out.assign(data, data + n);
    ok = true;
  } else if (gerr) {
    err = gerr->message ? gerr->message : "export failed";
    g_error_free(gerr);
  } else {
    err = "export failed (HTTP " + std::to_string(status) + ")";
  }
  if (bytes) g_bytes_unref(bytes);
  g_object_unref(msg);
  g_object_unref(session);
  return ok;
}

#else // !EH_HAVE_DRIVE — honest stubs so the UI still builds.

bool drive_exchange_code(const std::string&, const std::string&,
                         const std::string&, const std::string&,
                         const std::string&, DriveToken&, std::string& err) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}
bool drive_refresh_token(const std::string&, const std::string&,
                         const std::string&, std::string&, int64_t&,
                         std::string& err) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}
bool drive_fetch_email(const std::string&, std::string&, std::string& err) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}
bool drive_list_children(const std::string&, const std::string&,
                         std::vector<DriveItem>&, std::string& err,
                         const std::function<bool()>&,
                         const std::string&) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}
bool drive_download(const std::string&, const std::string&,
                    std::vector<uint8_t>&, std::string& err,
                    const std::string&) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}
bool drive_export(const std::string&, const std::string&, const std::string&,
                  std::vector<uint8_t>&, std::string& err,
                  const std::string&) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}

#endif // EH_HAVE_DRIVE


const DriveAccount* drive_find(const AppState& app,
                                         const std::string& email) {
  for (auto& a : app.drive_accounts)
    if (a.email == email) return &a;
  return nullptr;
}

DriveAccount* drive_find(AppState& app, const std::string& email) {
  for (auto& a : app.drive_accounts)
    if (a.email == email) return &a;
  return nullptr;
}

bool drive_ensure_token(AppState& app, const std::string& email,
                        std::string& access_out, std::string& err) {
  std::string refresh, access;
  int64_t expiry = 0;
  std::string client_id, client_secret;
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    DriveAccount* acc = drive_find(app, email);
    if (!acc) {
      err = "Google Drive account not connected";
      return false;
    }
    if (!acc->access_token.empty() && now_ms() < acc->access_expiry_ms - 60000) {
      access_out = acc->access_token;
      return true;
    }
    refresh = acc->refresh_token;
    client_id = acc->client_id;
    client_secret = acc->client_secret;
  }
  if (refresh.empty()) {
    err = "Drive session expired — reconnect the account";
    return false;
  }
  std::string fresh;
  int64_t expires_in = 0;
  if (!drive_refresh_token(client_id, client_secret, refresh, fresh,
                           expires_in, err))
    return false;
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    DriveAccount* acc = drive_find(app, email);
    if (!acc) {
      err = "Google Drive account disconnected";
      return false;
    }
    acc->access_token = fresh;
    acc->access_expiry_ms =
        now_ms() + (expires_in > 120 ? expires_in - 60 : expires_in) * 1000;
    access_out = fresh;
  }
  (void)access;
  (void)expiry;
  return true;
}

std::string drive_resolve_id(AppState& app, const std::string& email,
                             const std::string& display_path,
                             std::string& err,
                             const std::function<bool()>& gen_done) {
  return drive_locate(app, email, display_path, err, gen_done).id;
}

#ifdef EH_HAVE_DRIVE

bool drive_list_shared_drives(const std::string& access_token,
                              std::vector<SharedDrive>& out, std::string& err) {
  std::string url_base =
      "https://www.googleapis.com/drive/v3/drives?pageSize=100&fields=" +
      form_escape("nextPageToken,drives(id,name)");
  std::string page;
  do {
    std::string url = url_base + (page.empty() ? "" : "&pageToken=" + page);
    int status = 0;
    std::string resp = soup_fetch(url, access_token, {}, status, err);
    if (resp.empty() || (status < 200 || status >= 300)) {
      if (err.empty()) err = "Cannot list shared drives";
      return false;
    }
    JsonParser* parser = json_parser_new();
    GError* gerr = nullptr;
    bool ok = false;
    page.clear();
    if (json_parser_load_from_data(parser, resp.c_str(),
                                   static_cast<gssize>(resp.size()), &gerr)) {
      JsonNode* root = json_parser_get_root(parser);
      JsonObject* obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                            ? json_node_get_object(root)
                            : nullptr;
      if (obj) {
        if (json_object_has_member(obj, "nextPageToken"))
          page = json_object_get_string_member(obj, "nextPageToken");
        JsonArray* arr = json_object_has_member(obj, "drives")
                             ? json_object_get_array_member(obj, "drives")
                             : nullptr;
        if (arr) {
          for (guint i = 0; i < json_array_get_length(arr); ++i) {
            JsonObject* d = json_array_get_object_element(arr, i);
            if (!d || !json_object_has_member(d, "id") ||
                !json_object_has_member(d, "name"))
              continue;
            SharedDrive sd;
            sd.id = json_object_get_string_member(d, "id");
            sd.name = json_object_get_string_member(d, "name");
            out.push_back(std::move(sd));
          }
        }
        ok = true;
      } else {
        err = "bad Drive response";
      }
    } else {
      err = gerr && gerr->message ? gerr->message : "bad Drive response";
      if (gerr) g_error_free(gerr);
    }
    g_object_unref(parser);
    if (!ok) return false;
  } while (!page.empty());
  return true;
}

#else // !EH_HAVE_DRIVE

bool drive_list_shared_drives(const std::string&, std::vector<SharedDrive>&,
                              std::string& err) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}

#endif // EH_HAVE_DRIVE

namespace {

// Walk segments [start, …) under start_id; raw_prefix is the raw path of
// the start ("" at account root). Corporate context via drive_id.
std::string walk_from(AppState& app, const std::string& email,
                      const std::string& access,
                      const std::vector<std::string>& segs, size_t start,
                      const std::string& start_id, const std::string& drive_id,
                      const std::string& raw_prefix, std::string& err,
                      const std::function<bool()>& gen_done) {
  std::string full;
  for (auto& s : segs) full += "/" + s;
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    DriveAccount* acc = drive_find(app, email);
    if (!acc) {
      err = "Google Drive account disconnected";
      return {};
    }
    auto it = acc->id_cache.find(full);
    if (it != acc->id_cache.end()) return it->second;
  }
  std::string cur_id = start_id;
  std::string cur_raw = raw_prefix;
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (DriveAccount* acc = drive_find(app, email)) {
      auto pit = acc->id_cache.find(cur_raw.empty() ? "/" : cur_raw);
      if (pit != acc->id_cache.end() && start_id == "root") {
        // Only trust the prefix cache for My-Drive walks from root;
        // shared contexts carry their start id explicitly.
        cur_id = pit->second;
      }
    }
  }
  for (size_t depth = start; depth < segs.size(); ++depth) {
    std::string want = unescape_seg(segs[depth]);
    std::vector<DriveItem> kids;
    std::string lerr;
    if (!drive_list_children(access, cur_id, kids, lerr, gen_done,
                             drive_id)) {
      if (!lerr.empty()) err = lerr; // empty = superseded, stay silent
      return {};
    }
    std::string next;
    for (auto& k : kids) {
      if (k.name != want) continue;
      if (depth + 1 < segs.size() && !k.is_folder) continue;
      next = k.id;
      break;
    }
    if (next.empty()) {
      err = "Not found in Drive: " + want;
      return {};
    }
    cur_id = next;
    cur_raw += "/" + segs[depth];
    {
      std::lock_guard<std::mutex> lk(app.scan_mtx);
      if (DriveAccount* acc = drive_find(app, email))
        acc->id_cache[cur_raw] = cur_id;
    }
  }
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (DriveAccount* acc = drive_find(app, email)) acc->id_cache[full] = cur_id;
  }
  return cur_id;
}

constexpr const char* kSharedPseudo = "Shared drives";

// Shared-corpus branch of locate (segs[0] is the pseudo folder).
DriveLoc locate_shared(AppState& app, const std::string& email,
                       const std::string& access,
                       const std::vector<std::string>& segs, std::string& err,
                       const std::function<bool()>& gen_done) {
  if (segs.size() == 1) return {"shared:roots", ""};
  std::string want = unescape_seg(segs[1]);
  std::string did;
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (DriveAccount* acc = drive_find(app, email)) {
      auto it = acc->id_cache.find("/Shared drives/" + segs[1]);
      if (it != acc->id_cache.end() &&
          it->second.rfind("shared:", 0) == 0)
        did = it->second.substr(std::char_traits<char>::length("shared:"));
    }
  }
  if (did.empty()) {
    std::vector<SharedDrive> drives;
    if (!drive_list_shared_drives(access, drives, err)) return {"", ""};
    for (auto& d : drives) {
      std::lock_guard<std::mutex> lk(app.scan_mtx);
      if (DriveAccount* acc = drive_find(app, email))
        acc->id_cache["/Shared drives/" + escape_seg(d.name)] =
            "shared:" + d.id;
      if (d.name == want) did = d.id;
    }
  }
  if (did.empty()) {
    err = "Not found in Drive: " + want;
    return {"", ""};
  }
  if (segs.size() == 2) return {"shared:" + did, did};
  std::string raw_prefix = "/Shared drives/" + segs[1];
  std::string id =
      walk_from(app, email, access, segs, 2, did, did, raw_prefix, err, gen_done);
  if (id.empty()) return {"", ""};
  return {id, did};
}

} // namespace

DriveLoc drive_locate(AppState& app, const std::string& email,
                      const std::string& display_path, std::string& err,
                      const std::function<bool()>& gen_done) {
  if (display_path.empty() || display_path == "/") return {"root", ""};
  std::string access;
  if (!drive_ensure_token(app, email, access, err)) return {"", ""};
  auto segs = split_raw(display_path);
  if (segs.empty()) return {"root", ""};
  // Fast path: a cached shared-drive marker avoids the My-Drive probing
  // walk entirely (no network).
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (DriveAccount* acc = drive_find(app, email)) {
      std::string full;
      for (auto& s : segs) full += "/" + s;
      auto it = acc->id_cache.find(full);
      if (it != acc->id_cache.end() &&
          it->second.rfind("shared:", 0) == 0 &&
          it->second != "shared:roots")
        return {it->second,
                it->second.substr(
                    std::char_traits<char>::length("shared:"))};
    }
  }
  // Real folders win over the pseudo name: walk My Drive first, fall back
  // to the shared-drives corpus only when it misses. A cached "shared:"
  // marker recovers its corpus from the marker itself (no network).
  std::string id =
      walk_from(app, email, access, segs, 0, "root", "", "", err, gen_done);
  if (!id.empty()) {
    constexpr const char* kMark = "shared:";
    if (id.rfind(kMark, 0) == 0 && id != "shared:roots")
      return {id, id.substr(std::char_traits<char>::length(kMark))};
    return {id, ""};
  }
  if (!segs.empty() && unescape_seg(segs[0]) == kSharedPseudo) {
    err.clear();
    return locate_shared(app, email, access, segs, err, gen_done);
  }
  return {"", ""};
}


FileEntry drive_entry_from_item(const std::string& email,
                                const std::string& dparent_raw,
                                const DriveItem& item) {
  FileEntry e;
  e.name = item.name;
  std::string child_raw =
      dparent_raw.empty() ? "/" + escape_seg(item.name)
                          : dparent_raw + "/" + escape_seg(item.name);
  e.path = "googledrive://" + email + child_raw;
  e.is_dir = item.is_folder;
  e.size = item.size;
  e.modified_sec = item.mtime_sec;
  e.readable = true;
  e.writable = true;
  e.is_hidden = !item.name.empty() && item.name[0] == '.';
  // Real mimes for uploaded files (Services/open-with filtering);
  // inode/directory for folders, like the remote worker.
  e.mime_type = item.is_folder ? "inode/directory" : item.mime;
  std::string ext_hint;
  if (!item.is_folder) {
    if (const GoogleKind* g = google_kind(item.mime)) {
      // Google-native file: icon classification by kind; the Ext column
      // only claims a format when we actually open it as one.
      ext_hint = g->ext_hint;
      if (g->export_mime) e.extension = ext_hint;
    }
    if (e.extension.empty()) {
      auto dot = item.name.rfind('.');
      if (dot != std::string::npos && dot + 1 < item.name.size()) {
        e.extension = item.name.substr(dot + 1);
        for (auto& c : e.extension)
          c = static_cast<char>(
              std::tolower(static_cast<unsigned char>(c)));
      }
    }
  }
  e.type = detect_file_type(item.name, item.is_folder, e.mime_type, "",
                            ext_hint);
  return e;
}

void reload_drive_dir(AppState& app) {
  app.scan_target_pane = app.active_pane ? 1 : 0;
  reset_preview(app);
  hide_tooltip(app);

  std::string uri = drive_normalize(app.cur_tab().current_path);
  app.cur_tab().current_path = uri;
  std::string email = drive_email(uri);
  std::string dpath = drive_display_path(uri);

  if (email.empty() || !drive_find(app, email)) {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    const uint64_t gen = ++app.scan_generation;
    app.scan_result.error = "Google Drive account not connected";
    app.scan_result.generation = gen;
    app.scan_result.path = uri;
    app.scan_result.entries.clear();
    app.scan_ready_flag.store(true, std::memory_order_release);
    draw(app);
    return;
  }

  remote_cancel_inflight(app);
  join_scan(app);

  const uint64_t gen = ++app.scan_generation;
  app.scan_cancel.store(false);
  app.scan_ready_flag.store(false);
  app.scan_active_path = uri;
  ScanParams sp;
  app.scan_active_params = sp;
  bool folders_first = app.folders_before_files;

  AppState* ap = &app;
  app.scan_thread = std::thread([ap, gen, uri, email, dpath, folders_first]() {
    std::vector<FileEntry> v;
    std::string err;
    auto stale = [&] {
      return ap->scan_generation != gen || ap->scan_cancel.load();
    };
    DriveLoc loc =
        drive_locate(*ap, email, dpath, err, [&] { return stale(); });
    if (!loc.id.empty() && !stale()) {
      std::string access;
      if (drive_ensure_token(*ap, email, access, err)) {
        std::vector<DriveItem> kids;
        if (loc.id == "shared:roots") {
          // Pseudo-folder: one entry per shared drive.
          std::vector<SharedDrive> drives;
          if (drive_list_shared_drives(access, drives, err) && !stale()) {
            for (auto& d : drives) {
              DriveItem k;
              k.id = "shared:" + d.id; // marker, never sent (see below)
              k.name = d.name;
              k.mime = kFolderMime;
              k.is_folder = true;
              kids.push_back(std::move(k));
            }
          } else if (err.empty()) {
            err = "Cannot list shared drives";
          }
        } else {
          // files.get check first: files.list on a file id returns an empty
          // list, indistinguishable from an empty folder.
          std::string check_err;
          bool is_folder = (loc.id == "root") ||
                           (loc.id.rfind("shared:", 0) == 0);
          if (!is_folder) {
            int status = 0;
#ifdef EH_HAVE_DRIVE
            {
              SoupSession* session = soup_session_new();
              std::string url = std::string(kFilesEndpoint) + "/" + loc.id +
                                "?fields=mimeType";
              drive_sup(url, loc.drive_id, false);
              SoupMessage* msg =
                  soup_message_new(SOUP_METHOD_GET, url.c_str());
            std::string got;
            if (msg) {
              soup_message_headers_append(
                  soup_message_get_request_headers(msg), "Authorization",
                  ("Bearer " + access).c_str());
              GError* gerr = nullptr;
              GBytes* bytes =
                  soup_session_send_and_read(session, msg, nullptr, &gerr);
              status = soup_message_get_status(msg);
              if (bytes) {
                gsize n = 0;
                const auto* data = static_cast<const char*>(
                    g_bytes_get_data(bytes, &n));
                if (data && n) got.assign(data, n);
                g_bytes_unref(bytes);
              }
              if (gerr) g_error_free(gerr);
              g_object_unref(msg);
            }
            g_object_unref(session);
            if (status >= 200 && status < 300 &&
                got.find("application/vnd.google-apps.folder") !=
                    std::string::npos)
              is_folder = true;
            else if (status == 404)
              check_err = "Not found in Drive";
            else if (!(status >= 200 && status < 300))
              check_err = "Drive lookup failed (HTTP " +
                          std::to_string(status) + ")";
          }
#else
          check_err = "Drive support not built";
#endif
        }
        if (!check_err.empty()) {
          err = check_err;
        } else if (!is_folder) {
          err = "Not a Drive folder";
        } else {
          // Shared-drive roots carry a "shared:" marker so the UI can
          // tell them apart; strip it for the API call.
          std::string list_id = loc.id;
          constexpr const char* kMark = "shared:";
          if (list_id.rfind(kMark, 0) == 0)
            list_id = list_id.substr(
                std::char_traits<char>::length(kMark));
          if (drive_list_children(access, list_id, kids, err,
                                  [&] { return stale(); },
                                  loc.drive_id) &&
              !stale()) {
          // My-Drive root also offers the shared-drives pseudo-folder
          // (unless a real folder already owns that name, which locate
          // prefers — keep both views consistent).
          if (loc.id == "root" && loc.drive_id.empty()) {
            bool have_real = false;
            for (auto& k : kids) {
              if (k.name == kSharedPseudo && k.is_folder) {
                have_real = true;
                break;
              }
            }
            if (!have_real) {
              std::vector<SharedDrive> drives;
              std::string derr;
              if (drive_list_shared_drives(access, drives, derr) &&
                  !drives.empty()) {
                DriveItem pseudo;
                pseudo.id = "shared:roots";
                pseudo.name = kSharedPseudo;
                pseudo.mime = kFolderMime;
                pseudo.is_folder = true;
                kids.push_back(std::move(pseudo));
              }
            }
          }
          for (auto& k : kids)
            v.push_back(drive_entry_from_item(email, dpath, k));
          std::sort(v.begin(), v.end(),
                    [folders_first](const FileEntry& a, const FileEntry& b) {
                      if (folders_first && a.is_dir != b.is_dir)
                        return a.is_dir > b.is_dir;
                      std::string al = a.name, bl = b.name;
                      for (auto& c : al)
                        c = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(c)));
                      for (auto& c : bl)
                        c = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(c)));
                      if (al != bl) return al < bl;
                      return a.name < b.name;
                    });
        }
      }
    }
    }
    }
    {
      std::lock_guard<std::mutex> lk(ap->scan_mtx);
      if (ap->scan_generation != gen || ap->scan_cancel.load()) return;
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


std::vector<SidebarLocation> drive_sidebar_rows(const AppState& app) {
  std::vector<SidebarLocation> rows;
  for (auto& a : app.drive_accounts) {
    SidebarLocation loc;
    loc.kind = SidebarLocation::Kind::Drive;
    loc.label = a.email;
    loc.icon_name = "folder-gdrive";
    loc.path = build_drive_uri(a.email, {});
    loc.drive_id = "drive:" + a.email;
    loc.is_mounted = true; // no mount step: click navigates straight in
    rows.push_back(std::move(loc));
  }
  return rows;
}

void drive_disconnect(AppState& app, const std::string& email) {
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    auto& accs = app.drive_accounts;
    for (auto it = accs.begin(); it != accs.end(); ++it) {
      if (it->email == email) {
        accs.erase(it);
        break;
      }
    }
  }
  drive_keyring_erase(email);
  // Flush live state first (merge preserves the email), then drop just it
  // from the file — a plain save would resurrect it via the merge above.
  save_file_browser_settings(app);
  {
    eh::config::FileBrowserSettings cur =
        eh::config::read_file_browser_toml();
    auto& v = cur.drive_account_emails;
    v.erase(std::remove(v.begin(), v.end(), email), v.end());
    (void)eh::config::write_file_browser_toml(cur);
  }
  // Leave any open Drive view with an honest error, then refresh.
  if (is_drive_uri(app.cur_tab().current_path) &&
      drive_email(app.cur_tab().current_path) == email)
    reload_drive_dir(app);
  refresh_sidebar(app);
  drive_toast(app, "Drive disconnected");
}


void drive_open_file(AppState& app, const FileEntry& entry) {
  std::string uri = entry.path;
  std::string email = drive_email(uri);
  std::string dpath = drive_display_path(uri);
  AppState* ap = &app;
  std::thread([ap, uri, email, dpath, name = entry.name,
               mime = entry.mime_type]() {
    std::string err;
    auto stale = [&] { return false; }; // open is one-shot, not cancellable
    // Google-native types export; binary-ish ones download as-is;
    // editor-only types (Forms/Sites/Maps/…) can't be opened here.
    const GoogleKind* gk = google_kind(mime);
    if (gk && !gk->export_mime && !gk->downloadable) {
      DeferredCall::callLater([ap]() {
        drive_toast(*ap, "That Google file type can't be opened");
      });
      return;
    }
    DriveLoc oloc = drive_locate(*ap, email, dpath, err, stale);
    const std::string& id = oloc.id;
    const std::string& did = oloc.drive_id;
    std::string access;
    if (!id.empty() && id != "shared:roots" && id.rfind("shared:", 0) != 0 &&
        drive_ensure_token(*ap, email, access, err)) {
      std::vector<uint8_t> bytes;
      bool fetched = (gk && gk->export_mime)
                         ? drive_export(access, id, gk->export_mime, bytes,
                                        err, did)
                         : drive_download(access, id, bytes, err, did);
      if (fetched) {
        if (bytes.size() > 256u * 1024u * 1024u) {
          err = "File too large to preview (256 MiB cap)";
        } else {
          std::string safe = name;
          for (auto& c : safe)
            if (c == '/') c = '_';
          if (safe.empty()) safe = "drive-file";
          if (gk && gk->export_mime) {
            // Match the temp name to the export format so the default
            // app is picked correctly.
            std::string lower = safe;
            for (auto& c : lower)
              c = static_cast<char>(std::tolower(
                  static_cast<unsigned char>(c)));
            std::string suf = gk->export_suffix;
            if (lower.size() < suf.size() ||
                lower.compare(lower.size() - suf.size(), suf.size(), suf) !=
                    0)
              safe += suf;
          }
          std::string tmp =
              std::string(g_get_tmp_dir()) + "/horizon-drive-" +
              std::to_string(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count()) +
              "-" + safe;
          FILE* f = fopen(tmp.c_str(), "wb");
          if (!f) {
            err = "Cannot write temp file";
          } else {
            size_t w = bytes.empty()
                           ? 0
                           : fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
            if (w != bytes.size()) {
              err = "Cannot write temp file";
            } else {
              pid_t pid = fork();
              if (pid == 0) {
                setsid();
                execlp("xdg-open", "xdg-open", tmp.c_str(), nullptr);
                _exit(127);
              }
              DeferredCall::callLater([ap]() {
                drive_toast(*ap, "Opening from Drive…");
              });
              return;
            }
          }
        }
      }
    }
    DeferredCall::callLater([ap, err]() {
      drive_toast(*ap, err.empty() ? "Cannot open Drive file" : err);
    });
  }).detach();
}


namespace {

// Parse "GET /?code=..&state=.. HTTP/1.1" → code/state/error. False = junk.
bool parse_callback(const std::string& head, std::string& code,
                    std::string& state, std::string& oerr) {
  auto eol = head.find("\r\n");
  std::string line = head.substr(0, eol);
  if (line.rfind("GET ", 0) != 0) return false;
  auto sp = line.find(' ', 4);
  std::string target = line.substr(4, sp == std::string::npos
                                          ? std::string::npos
                                          : sp - 4);
  auto qm = target.find('?');
  if (qm == std::string::npos) return false;
  std::string q = target.substr(qm + 1);
  size_t i = 0;
  while (i <= q.size()) {
    size_t j = q.find('&', i);
    if (j == std::string::npos) j = q.size();
    std::string kv = q.substr(i, j - i);
    auto eq = kv.find('=');
    std::string k = kv.substr(0, eq);
    std::string v = eq == std::string::npos ? "" : kv.substr(eq + 1);
    gchar* u = g_uri_unescape_string(v.c_str(), nullptr);
    std::string uv = u ? u : v;
    g_free(u);
    if (k == "code") code = uv;
    else if (k == "state") state = uv;
    else if (k == "error") oerr = uv;
    else if (k == "error_description" && oerr.empty()) oerr = uv;
    if (j == q.size()) break;
    i = j + 1;
  }
  return true;
}

void http_reply(int fd, bool ok) {  std::string body = ok ? "<html><body style='font-family:sans-serif'>"
                          "<h2>Signed in.</h2><p>You can close this tab and "
                          "return to Horizon Files.</p></body></html>"
                        : "<html><body><h2>Sign-in failed.</h2><p>You can "
                          "close this tab.</p></body></html>";
  std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                     "Content-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
  std::string all = head + body;
  size_t off = 0;
  while (off < all.size()) {
    ssize_t w = send(fd, all.data() + off, all.size() - off, MSG_NOSIGNAL);
    if (w <= 0) break;
    off += static_cast<size_t>(w);
  }
}

} // namespace

void drive_connect_account(AppState& app, std::string client_id,
                           std::string client_secret) {
  AppState* ap = &app;
  std::thread([ap, client_id, client_secret]() {
    auto fail = [ap](const std::string& msg) {
      DeferredCall::callLater([ap, msg]() { drive_toast(*ap, msg); });
    };
    if (client_id.empty()) {
      fail("Enter a Google OAuth client ID first");
      return;
    }
    // 1. Loopback listener on an ephemeral port.
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
      fail("Cannot listen for sign-in reply");
      return;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(srv, 1) != 0) {
      close(srv);
      fail("Cannot listen for sign-in reply");
      return;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &alen) != 0) {
      close(srv);
      fail("Cannot listen for sign-in reply");
      return;
    }
    int port = ntohs(addr.sin_port);
    std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/";
    std::string verifier = drive_random_verifier();
    std::string state = drive_random_state();
    std::string url =
        drive_auth_url(client_id, redirect, state, drive_code_challenge(verifier));

    // 2. System browser.
    {
      pid_t pid = fork();
      if (pid < 0) {
        close(srv);
        fail("Cannot open the browser");
        return;
      }
      if (pid == 0) {
        setsid();
        execlp("xdg-open", "xdg-open", url.c_str(), nullptr);
        _exit(127);
      }
    }
    DeferredCall::callLater([ap]() {
      drive_toast(*ap, "Browser opened — sign in to Google…");
    });

    // 3. Wait for the redirect (5 minutes).
    std::string code, got_state, oerr;
    bool got_any = false;
    {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(srv, &rfds);
      timeval tv{300, 0};
      if (select(srv + 1, &rfds, nullptr, nullptr, &tv) > 0) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd >= 0) {
          std::string head;
          char buf[4096];
          while (head.size() < 65536) {
            ssize_t r = recv(fd, buf, sizeof(buf), 0);
            if (r <= 0) break;
            head.append(buf, static_cast<size_t>(r));
            if (head.find("\r\n\r\n") != std::string::npos) break;
          }
          got_any = parse_callback(head, code, got_state, oerr);
          http_reply(fd, got_any && !code.empty() && got_state == state);
          close(fd);
        }
      }
    }
    close(srv);
    if (!got_any || code.empty()) {
      fail(oerr.empty() ? "Sign-in timed out or was dismissed" : oerr);
      return;
    }
    if (got_state != state) {
      fail("Sign-in reply mismatch — try again");
      return;
    }

    // 4. Exchange + identify.
    DriveToken tok;
    std::string err;
    if (!drive_exchange_code(client_id, client_secret, code, verifier, redirect,
                             tok, err)) {
      // A "client_secret is missing" refusal means the Cloud Console entry
      // is a Web-application client (confidential). We only speak public
      // Desktop clients (PKCE, no secret) — say so instead of parroting 400.
      if (err.find("client_secret") != std::string::npos &&
          client_secret.empty())
        fail("That client ID needs a secret — create a Desktop-type client");
      else
        fail(err.empty() ? "Sign-in failed" : err);
      return;
    }
    std::string email;
    if (!drive_fetch_email(tok.access_token, email, err)) {
      fail(err.empty() ? "Cannot read account email" : err);
      return;
    }
    // Tokens never touch disk: persist the client ID only.
    DeferredCall::callLater([ap, email, tok, client_id, client_secret]() {
      ap->drive_client_id = client_id;
      ap->drive_client_secret = client_secret; // memory only, never saved
      {
        std::lock_guard<std::mutex> lk(ap->scan_mtx);
        bool replaced = false;
        for (auto& a : ap->drive_accounts) {
          if (a.email == email) {
            a.refresh_token = tok.refresh_token.empty()
                                  ? a.refresh_token
                                  : tok.refresh_token;
            a.access_token = tok.access_token;
            a.access_expiry_ms =
                now_ms() +
                (tok.expires_in_sec > 120 ? tok.expires_in_sec - 60
                                          : tok.expires_in_sec) *
                    1000;
            a.client_id = client_id;
            if (!client_secret.empty()) a.client_secret = client_secret;
            a.id_cache.clear();
            replaced = true;
            break;
          }
        }
        if (!replaced) {
          DriveAccount acc;
          acc.email = email;
          acc.refresh_token = tok.refresh_token;
          acc.access_token = tok.access_token;
          acc.access_expiry_ms =
              now_ms() +
              (tok.expires_in_sec > 120 ? tok.expires_in_sec - 60
                                        : tok.expires_in_sec) *
                  1000;
          acc.client_id = client_id;
          acc.client_secret = client_secret;
          ap->drive_accounts.push_back(std::move(acc));
        }
      }
      // Remember the login: refresh token to the keyring, email to config.
      if (!tok.refresh_token.empty())
        drive_keyring_store(email, client_id, client_secret, tok.refresh_token);
      save_file_browser_settings(*ap);
      refresh_sidebar(*ap);
      drive_toast(*ap, "Drive connected: " + email);
      navigate_to(*ap, build_drive_uri(email, {}));
      draw(*ap);
    });
  }).detach();
}


namespace {
AppState* g_drive_app = nullptr;
std::mutex g_drive_app_mtx;
} // namespace

void drive_bind_app(AppState& app) {
  std::lock_guard<std::mutex> lk(g_drive_app_mtx);
  g_drive_app = &app;
}

#ifdef EH_HAVE_DRIVE
namespace {
AppState* drive_bound_app() {
  std::lock_guard<std::mutex> lk(g_drive_app_mtx);
  return g_drive_app;
}
} // namespace
#endif // EH_HAVE_DRIVE

#ifdef EH_HAVE_DRIVE

namespace {

// JSON POST/PATCH/DELETE with bearer auth (worker threads).
bool drive_json_call(const std::string& method, const std::string& url,
                     const std::string& access, const std::string& body,
                     std::string& resp_out, int& status_out,
                     std::string& err) {
  SoupSession* session = soup_session_new();
  SoupMessage* msg = soup_message_new(method.c_str(), url.c_str());
  if (!msg) {
    err = "cannot build request";
    g_object_unref(session);
    return false;
  }
  soup_message_headers_append(soup_message_get_request_headers(msg),
                              "Authorization",
                              soup_bearer(access).c_str());
  if (!body.empty()) {
    GBytes* b = g_bytes_new(body.data(), body.size());
    soup_message_set_request_body_from_bytes(msg, "application/json", b);
    g_bytes_unref(b);
  }
  GError* gerr = nullptr;
  GBytes* bytes = soup_session_send_and_read(session, msg, nullptr, &gerr);
  status_out = soup_message_get_status(msg);
  if (bytes) {
    gsize n = 0;
    const auto* data =
        static_cast<const char*>(g_bytes_get_data(bytes, &n));
    if (data && n) resp_out.assign(data, n);
    g_bytes_unref(bytes);
  }
  bool ok = status_out >= 200 && status_out < 300;
  if (!ok) {
    if (gerr && gerr->message)
      err = gerr->message;
    else
      err = "Drive request failed (HTTP " + std::to_string(status_out) + ")";
    // 404 is routine (existence checks) — don't spam the log for it.
    if (!resp_out.empty() && status_out != 404)
      fprintf(stderr, "horizon-files: drive %s -> HTTP %d %.200s\n",
              url.c_str(), status_out, resp_out.c_str());
  }
  if (gerr) g_error_free(gerr);
  g_object_unref(msg);
  g_object_unref(session);
  return ok;
}

bool drive_parse_id(const std::string& body, std::string& id_out,
                    std::string& err) {
  JsonParser* parser = json_parser_new();
  GError* gerr = nullptr;
  bool ok = false;
  if (json_parser_load_from_data(parser, body.c_str(),
                                 static_cast<gssize>(body.size()), &gerr)) {
    JsonNode* root = json_parser_get_root(parser);
    JsonObject* obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                          ? json_node_get_object(root)
                          : nullptr;
    if (obj && json_object_has_member(obj, "id")) {
      id_out = json_object_get_string_member(obj, "id");
      ok = !id_out.empty();
    } else if (obj && json_object_has_member(obj, "error")) {
      JsonObject* e = json_object_get_object_member(obj, "error");
      const char* m = e && json_object_has_member(e, "message")
                          ? json_object_get_string_member(e, "message")
                          : nullptr;
      err = m ? m : "Drive request refused";
    } else {
      err = "bad Drive response";
    }
  } else {
    err = gerr && gerr->message ? gerr->message : "bad Drive response";
    if (gerr) g_error_free(gerr);
  }
  g_object_unref(parser);
  return ok;
}

std::string drive_json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

} // namespace

bool drive_api_mkdir(const std::string& access, const std::string& parent_id,
                     const std::string& name, std::string& id_out,
                     std::string& err, const std::string& drive_id) {
  std::string body = "{\"name\":\"" + drive_json_escape(name) +
                     "\",\"mimeType\":\"" + kFolderMime +
                     "\",\"parents\":[\"" + parent_id + "\"]}";
  std::string resp;
  int status = 0;
  std::string url = kFilesEndpoint;
  drive_sup(url, drive_id, false);
  if (!drive_json_call("POST", url, access, body, resp, status, err))
    return false;
  return drive_parse_id(resp, id_out, err);
}

bool drive_api_create_empty(const std::string& access,
                            const std::string& parent_id,
                            const std::string& name, const std::string& mime,
                            std::string& id_out, std::string& err,
                            const std::string& drive_id) {
  std::string body = "{\"name\":\"" + drive_json_escape(name) +
                     "\",\"mimeType\":\"" + drive_json_escape(mime) +
                     "\",\"parents\":[\"" + parent_id + "\"]}";
  std::string resp;
  int status = 0;
  std::string url = kFilesEndpoint;
  drive_sup(url, drive_id, false);
  if (!drive_json_call("POST", url, access, body, resp, status, err))
    return false;
  return drive_parse_id(resp, id_out, err);
}

bool drive_api_trash(const std::string& access, const std::string& id,
                     std::string& err, const std::string& drive_id) {
  std::string resp;
  int status = 0;
  std::string url = std::string(kFilesEndpoint) + "/" + id;
  drive_sup(url, drive_id, false);
  return drive_json_call("PATCH", url, access, "{\"trashed\":true}", resp,
                         status, err);
}

bool drive_api_delete(const std::string& access, const std::string& id,
                      std::string& err, const std::string& drive_id) {
  std::string resp;
  int status = 0;
  std::string url = std::string(kFilesEndpoint) + "/" + id;
  drive_sup(url, drive_id, false);
  return drive_json_call("DELETE", url, access, {}, resp, status, err);
}

bool drive_api_move(const std::string& access, const std::string& id,
                    const std::string& name, const std::string& new_parent,
                    const std::string& old_parent, std::string& err,
                    const std::string& drive_id) {
  std::string url = std::string(kFilesEndpoint) + "/" + id + "?addParents=" +
                    new_parent + "&removeParents=" + old_parent;
  drive_sup(url, drive_id, false);
  std::string body = name.empty() ? "{}"
                                  : "{\"name\":\"" + drive_json_escape(name) +
                                        "\"}";
  std::string resp;
  int status = 0;
  return drive_json_call("PATCH", url, access, body, resp, status, err);
}

bool drive_api_copy(const std::string& access, const std::string& src_id,
                    const std::string& dst_parent, const std::string& name,
                    std::string& id_out, std::string& err,
                    const std::string& drive_id) {
  std::string body = "{\"name\":\"" + drive_json_escape(name) +
                     "\",\"parents\":[\"" + dst_parent + "\"]}";
  std::string resp;
  int status = 0;
  std::string url = std::string(kFilesEndpoint) + "/" + src_id + "/copy";
  drive_sup(url, drive_id, false);
  if (!drive_json_call("POST", url, access, body, resp, status, err))
    return false;
  return drive_parse_id(resp, id_out, err);
}

bool drive_api_stat(const std::string& access, const std::string& id,
                    DriveStat& out, std::string& err,
                    const std::string& drive_id) {
  std::string url = std::string(kFilesEndpoint) + "/" + id +
                    "?fields=id,mimeType,size,modifiedTime,trashed";
  drive_sup(url, drive_id, false);
  std::string resp;
  int status = 0;
  if (!drive_json_call("GET", url, access, {}, resp, status, err))
    return false;
  JsonParser* parser = json_parser_new();
  GError* gerr = nullptr;
  bool ok = false;
  if (json_parser_load_from_data(parser, resp.c_str(),
                                 static_cast<gssize>(resp.size()), &gerr)) {
    JsonNode* root = json_parser_get_root(parser);
    JsonObject* obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                          ? json_node_get_object(root)
                          : nullptr;
    if (obj) {
      bool trashed = json_object_has_member(obj, "trashed") &&
                     json_object_get_boolean_member(obj, "trashed");
      if (!trashed) {
        out.exists = true;
        const char* mime = json_object_has_member(obj, "mimeType")
                               ? json_object_get_string_member(obj, "mimeType")
                               : "";
        out.is_dir = mime == std::string(kFolderMime);
        if (!out.is_dir && json_object_has_member(obj, "size")) {
          try {
            out.size = std::stoull(json_object_get_string_member(obj, "size"));
          } catch (...) {
          }
        }
        if (json_object_has_member(obj, "modifiedTime")) {
          GDateTime* dt = g_date_time_new_from_iso8601(
              json_object_get_string_member(obj, "modifiedTime"), nullptr);
          if (dt) {
            out.mtime = g_date_time_to_unix(dt);
            g_date_time_unref(dt);
          }
        }
        ok = true;
      }
    } else {
      err = "bad Drive response";
    }
  } else {
    err = gerr && gerr->message ? gerr->message : "bad Drive response";
    if (gerr) g_error_free(gerr);
  }
  g_object_unref(parser);
  return ok;
}

#endif // EH_HAVE_DRIVE


#ifdef EH_HAVE_DRIVE
// Defined below (transfers section); declared here for the wrappers.
bool drive_api_mkdir(const std::string& access, const std::string& parent_id,
                     const std::string& name, std::string& id_out,
                     std::string& err, const std::string& drive_id = "");
bool drive_api_create_empty(const std::string& access,
                            const std::string& parent_id,
                            const std::string& name, const std::string& mime,
                            std::string& id_out, std::string& err,
                            const std::string& drive_id = "");
bool drive_api_trash(const std::string& access, const std::string& id,
                     std::string& err, const std::string& drive_id = "");
bool drive_api_delete(const std::string& access, const std::string& id,
                      std::string& err, const std::string& drive_id = "");
bool drive_api_move(const std::string& access, const std::string& id,
                    const std::string& name, const std::string& new_parent,
                    const std::string& old_parent, std::string& err,
                    const std::string& drive_id = "");
bool drive_api_copy(const std::string& access, const std::string& src_id,
                    const std::string& dst_parent, const std::string& name,
                    std::string& id_out, std::string& err,
                    const std::string& drive_id = "");
bool drive_api_stat(const std::string& access, const std::string& id,
                    DriveStat& out, std::string& err,
                    const std::string& drive_id = "");
#endif

#ifdef EH_HAVE_DRIVE
namespace {
bool drive_path_creds(const std::string& email, AppState*& ap_out,
                      std::string& access, std::string& err) {
  AppState* ap = drive_bound_app();
  if (!ap) {
    err = "Drive unavailable";
    return false;
  }
  if (!drive_ensure_token(*ap, email, access, err)) return false;
  ap_out = ap;
  return true;
}
} // namespace
#endif // EH_HAVE_DRIVE

bool drive_stat_uri(const std::string& uri, DriveStat& out,
                    std::string& err) {
  out = DriveStat{};
  if (!is_drive_uri(uri)) {
    err = "not a Drive location";
    return false;
  }
#ifdef EH_HAVE_DRIVE
  AppState* ap = nullptr;
  std::string access;
  if (!drive_path_creds(drive_email(uri), ap, access, err)) return false;
  DriveLoc loc = drive_locate(*ap, drive_email(uri),
                                drive_display_path(uri), err,
                                [] { return false; });
  if (loc.id.empty()) return true; // missing: not-exists
  if (loc.id == "root" || loc.id == "shared:roots" ||
      loc.id.rfind("shared:", 0) == 0) {
    out.exists = true; // roots and shared-drive roots always exist
    out.is_dir = true;
    return true;
  }
  if (!drive_api_stat(access, loc.id, out, err, loc.drive_id)) {
    out = DriveStat{};
    return true; // stat failure reads as missing to vfs callers
  }
  return true;
#else
  (void)err;
  err = "Drive support not built";
  return false;
#endif
}

#ifdef EH_HAVE_DRIVE
// mkdir -p for a raw display path: longest locatable prefix wins, the rest
// is created level by level. Returns the leaf id + corpus (did "" = My
// Drive). Refuses the "/Shared drives" pseudo-root itself.
static bool drive_ensure_path(AppState& app, const std::string& email,
                              const std::string& access,
                              const std::string& raw_dpath,
                              std::string& leaf_out, std::string& did_out,
                              std::string& err,
                              const std::function<bool()>& gen_done) {
  if (raw_dpath.empty() || raw_dpath == "/") {
    leaf_out = "root";
    did_out = "";
    return true;
  }
  auto segs = split_raw(raw_dpath);
  for (size_t len = segs.size(); len >= 1; --len) {
    std::string prefix;
    for (size_t i = 0; i < len; ++i) prefix += "/" + segs[i];
    std::string lerr;
    DriveLoc loc = drive_locate(app, email, prefix, lerr, gen_done);
    if (loc.id.empty() || loc.id == "shared:roots") continue;
    std::string cur = loc.id;
    std::string cur_raw = prefix;
    bool ok = true;
    for (size_t i = len; i < segs.size(); ++i) {
      std::string next;
      if (!drive_api_mkdir(access, cur, unescape_seg(segs[i]), next, err,
                           loc.drive_id)) {
        ok = false;
        break;
      }
      cur = next;
      cur_raw += "/" + segs[i];
      {
        std::lock_guard<std::mutex> lk(app.scan_mtx);
        if (DriveAccount* acc = drive_find(app, email))
          acc->id_cache[cur_raw] = cur;
      }
    }
    if (!ok) return false;
    leaf_out = cur;
    did_out = loc.drive_id;
    return true;
  }
  err = "Cannot create a folder here";
  return false;
}
#endif // EH_HAVE_DRIVE

bool drive_mkdir_path(const std::string& uri, std::string& err) {
  if (!is_drive_uri(uri)) {
    err = "not a Drive location";
    return false;
  }
#ifdef EH_HAVE_DRIVE
  AppState* ap = nullptr;
  std::string access;
  std::string email = drive_email(uri);
  if (!drive_path_creds(email, ap, access, err)) return false;
  // mkdir -p: the chain creates whatever is missing (idempotent).
  std::string leaf, did;
  return drive_ensure_path(*ap, email, access, drive_display_path(uri), leaf,
                           did, err, [] { return false; });
#else
  (void)err;
  err = "Drive support not built";
  return false;
#endif
}

bool drive_create_empty_path(const std::string& uri, const std::string& mime,
                             std::string& err) {
  if (!is_drive_uri(uri)) {
    err = "not a Drive location";
    return false;
  }
#ifdef EH_HAVE_DRIVE
  AppState* ap = nullptr;
  std::string access;
  std::string email = drive_email(uri);
  if (!drive_path_creds(email, ap, access, err)) return false;
  std::string dpath = drive_display_path(uri);
  std::string parent_raw = dpath;
  auto slash = parent_raw.rfind('/');
  std::string name_raw =
      (slash == std::string::npos) ? parent_raw : parent_raw.substr(slash + 1);
  std::string name = unescape_seg(name_raw);
  parent_raw =
      (slash == std::string::npos || slash == 0) ? "" : parent_raw.substr(0, slash);
  if (name.empty()) {
    err = "Cannot create a file here";
    return false;
  }
  std::string parent_id, parent_did;
  if (!drive_ensure_path(*ap, email, access, parent_raw, parent_id,
                         parent_did, err, [] { return false; }))
    return false;
  std::string id;
  std::string use_mime = mime.empty() ? "text/plain" : mime;
  return drive_api_create_empty(access, parent_id, name, use_mime, id, err,
                                parent_did);
#else
  (void)mime;
  (void)err;
  err = "Drive support not built";
  return false;
#endif
}

bool drive_trash_path(const std::string& uri, std::string& err) {
  if (!is_drive_uri(uri)) {
    err = "not a Drive location";
    return false;
  }
#ifdef EH_HAVE_DRIVE
  AppState* ap = nullptr;
  std::string access;
  std::string email = drive_email(uri);
  if (!drive_path_creds(email, ap, access, err)) return false;
  DriveLoc loc = drive_locate(*ap, email, drive_display_path(uri), err,
                              [] { return false; });
  if (loc.id.empty() || loc.id == "root" || loc.id == "shared:roots" ||
      loc.id.rfind("shared:", 0) == 0) {
    if (err.empty()) err = "Cannot trash the Drive root";
    return false;
  }
  return drive_api_trash(access, loc.id, err, loc.drive_id);
#else
  (void)err;
  err = "Drive support not built";
  return false;
#endif
}

bool drive_delete_path(const std::string& uri, std::string& err) {
  if (!is_drive_uri(uri)) {
    err = "not a Drive location";
    return false;
  }
#ifdef EH_HAVE_DRIVE
  AppState* ap = nullptr;
  std::string access;
  std::string email = drive_email(uri);
  if (!drive_path_creds(email, ap, access, err)) return false;
  DriveLoc loc = drive_locate(*ap, email, drive_display_path(uri), err,
                              [] { return false; });
  if (loc.id.empty() || loc.id == "root" || loc.id == "shared:roots" ||
      loc.id.rfind("shared:", 0) == 0) {
    if (err.empty()) err = "Cannot delete the Drive root";
    return false;
  }
  return drive_api_delete(access, loc.id, err, loc.drive_id);
#else
  (void)err;
  err = "Drive support not built";
  return false;
#endif
}

bool drive_move_path(const std::string& from_uri, const std::string& to_uri,
                     std::string& err) {
  if (!is_drive_uri(from_uri) || !is_drive_uri(to_uri)) {
    err = "not a Drive location";
    return false;
  }
  if (drive_email(from_uri) != drive_email(to_uri)) {
    err = "Cannot move across Drive accounts — copy instead";
    return false;
  }
#ifdef EH_HAVE_DRIVE
  AppState* ap = nullptr;
  std::string access;
  std::string email = drive_email(from_uri);
  if (!drive_path_creds(email, ap, access, err)) return false;
  std::string from_d = drive_display_path(from_uri);
  std::string to_d = drive_display_path(to_uri);
  DriveLoc src_loc =
      drive_locate(*ap, email, from_d, err, [] { return false; });
  if (src_loc.id.empty() || src_loc.id == "root" ||
      src_loc.id == "shared:roots" ||
      src_loc.id.rfind("shared:", 0) == 0) {
    if (err.empty()) err = "Not found in Drive";
    return false;
  }
  // Resolve (mkdir -p) the destination parent.
  std::string to_parent = to_d;
  auto slash = to_parent.rfind('/');
  std::string to_name_raw = (slash == std::string::npos)
                                ? to_parent
                                : to_parent.substr(slash + 1);
  to_parent = (slash == std::string::npos || slash == 0)
                  ? ""
                  : to_parent.substr(0, slash);
  std::string to_parent_id, to_did;
  if (!drive_ensure_path(*ap, email, access, to_parent, to_parent_id, to_did,
                         err, [] { return false; }))
    return false;
  std::string from_parent = from_d;
  auto fslash = from_parent.rfind('/');
  from_parent = (fslash == std::string::npos || fslash == 0)
                    ? ""
                    : from_parent.substr(0, fslash);
  std::string from_parent_id, from_did;
  if (!drive_ensure_path(*ap, email, access, from_parent, from_parent_id,
                         from_did, err, [] { return false; }))
    return false;
  {
    std::lock_guard<std::mutex> lk(ap->scan_mtx);
    if (DriveAccount* acc = drive_find(*ap, email)) acc->id_cache.clear();
  }
  const std::string& did =
      !src_loc.drive_id.empty() ? src_loc.drive_id : to_did;
  return drive_api_move(access, src_loc.id, unescape_seg(to_name_raw),
                        to_parent_id, from_parent_id, err, did);
#else
  (void)err;
  err = "Drive support not built";
  return false;
#endif
}


#ifdef EH_HAVE_DRIVE

bool drive_upload_file(const std::string& access, const std::string& parent_id,
                       const std::string& name, const std::string& mime,
                       const std::string& local_path, OperationProgress* prog,
                       const std::function<bool()>& cancel,
                       std::string& id_out, std::string& err,
                       const std::string& drive_id) {
  std::error_code ec;
  uint64_t size = fs::file_size(local_path, ec);
  if (ec) {
    err = "Cannot read file";
    return false;
  }
  FILE* f = fopen(local_path.c_str(), "rb");
  if (!f) {
    err = "Cannot read file";
    return false;
  }
  bool ok = false;
  SoupSession* session = soup_session_new();
  // 1. Resumable session.
  std::string session_uri;
  {
    std::string url =
        "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable";
    drive_sup(url, drive_id, false);
    SoupMessage* init = soup_message_new("POST", url.c_str());
    if (!init) {
      err = "cannot build request";
      fclose(f);
      g_object_unref(session);
      return false;
    }
    soup_message_headers_append(soup_message_get_request_headers(init),
                                "Authorization",
                                soup_bearer(access).c_str());
    std::string body = "{\"name\":\"" + drive_json_escape(name) +
                       "\",\"parents\":[\"" + parent_id +
                       "\"],\"mimeType\":\"" + drive_json_escape(mime) + "\"}";
    GBytes* b = g_bytes_new(body.data(), body.size());
    soup_message_set_request_body_from_bytes(init, "application/json", b);
    g_bytes_unref(b);
    SoupMessageHeaders* rh = soup_message_get_request_headers(init);
    soup_message_headers_append(rh, "X-Upload-Content-Type", mime.c_str());
    soup_message_headers_append(
        rh, "X-Upload-Content-Length", std::to_string(size).c_str());
    GError* gerr = nullptr;
    GBytes* resp = soup_session_send_and_read(session, init, nullptr, &gerr);
    int status = soup_message_get_status(init);
    if (resp) g_bytes_unref(resp);
    if (status >= 200 && status < 300) {
      const char* loc = soup_message_headers_get_one(
          soup_message_get_response_headers(init), "Location");
      if (loc) session_uri = loc;
    } else if (gerr && gerr->message) {
      err = gerr->message;
    } else {
      err = "Upload failed (HTTP " + std::to_string(status) + ")";
    }
    if (gerr) g_error_free(gerr);
    g_object_unref(init);
  }
  // 2. Chunked PUT (1 MiB; empty file = single zero-length final chunk).
  static constexpr uint64_t kChunk = 1024u * 1024u;
  uint64_t off = 0;
  std::vector<char> buf(static_cast<size_t>(kChunk));
  if (!session_uri.empty()) {
    ok = true;
    while (off < size || (size == 0 && off == 0)) {
      if (cancel && cancel()) {
        ok = false;
        err.clear();
        break;
      }
      uint64_t want =
          (size == 0) ? 0 : std::min<uint64_t>(kChunk, size - off);
      size_t got = 0;
      if (want > 0) {
        got = fread(buf.data(), 1, static_cast<size_t>(want), f);
        if (got == 0) {
          err = "Cannot read file";
          ok = false;
          break;
        }
      }
      SoupMessage* put = soup_message_new("PUT", session_uri.c_str());
      if (!put) {
        err = "cannot build request";
        ok = false;
        break;
      }
      std::string range =
          (size == 0) ? "bytes */0"
                      : "bytes " + std::to_string(off) + "-" +
                            std::to_string(off + got - 1) + "/" +
                            std::to_string(size);
      SoupMessageHeaders* ph = soup_message_get_request_headers(put);
      soup_message_headers_append(ph, "Content-Range", range.c_str());
      // Session URIs are pre-authenticated capability URLs.
      GBytes* chunk = g_bytes_new(want == 0 ? nullptr : buf.data(), got);
      soup_message_set_request_body_from_bytes(put, "application/octet-stream",
                                               chunk);
      g_bytes_unref(chunk);
      GError* gerr = nullptr;
      GBytes* resp = soup_session_send_and_read(session, put, nullptr, &gerr);
      int status = soup_message_get_status(put);
      std::string rbody;
      if (resp) {
        gsize n = 0;
        const auto* data =
            static_cast<const char*>(g_bytes_get_data(resp, &n));
        if (data && n) rbody.assign(data, n);
        g_bytes_unref(resp);
      }
      if (status == 308) {
        off += got;
        if (prog) prog->done_bytes.fetch_add(got);
      } else if (status >= 200 && status < 300) {
        off += got;
        if (prog) prog->done_bytes.fetch_add(got);
        if (!drive_parse_id(rbody, id_out, err)) ok = false;
        break;
      } else if (gerr && gerr->message) {
        err = gerr->message;
        ok = false;
      } else {
        err = "Upload failed (HTTP " + std::to_string(status) + ")";
        ok = false;
      }
      if (gerr) g_error_free(gerr);
      g_object_unref(put);
      if (!ok) break;
      if (size == 0) break; // single zero-length chunk done
    }
  }
  fclose(f);
  g_object_unref(session);
  return ok && !id_out.empty();
}

bool drive_download_to(const std::string& access, const std::string& file_id,
                       const std::string& export_mime,
                       const std::string& local_path, OperationProgress* prog,
                       const std::function<bool()>& cancel,
                       std::string& err, const std::string& drive_id) {
  std::string url = std::string(kFilesEndpoint) + "/" + file_id +
                    (export_mime.empty()
                         ? "?alt=media"
                         : "/export?mimeType=" + form_escape(export_mime));
  drive_sup(url, drive_id, false);
  SoupSession* session = soup_session_new();
  SoupMessage* msg = soup_message_new("GET", url.c_str());
  if (!msg) {
    err = "cannot build request";
    g_object_unref(session);
    return false;
  }
  soup_message_headers_append(soup_message_get_request_headers(msg),
                              "Authorization",
                              soup_bearer(access).c_str());
  GError* gerr = nullptr;
  GInputStream* in = soup_session_send(session, msg, nullptr, &gerr);
  int status = soup_message_get_status(msg);
  bool ok = false;
  if (!in || status < 200 || status >= 300) {
    if (gerr && gerr->message)
      err = gerr->message;
    else
      err = "Download failed (HTTP " + std::to_string(status) + ")";
  } else {
    FILE* f = fopen(local_path.c_str(), "wb");
    if (!f) {
      err = "Cannot write file";
    } else {
      ok = true;
      char buf[65536];
      for (;;) {
        if (cancel && cancel()) {
          ok = false;
          err.clear();
          break;
        }
        GError* rerr = nullptr;
        gssize r = g_input_stream_read(in, buf, sizeof(buf), nullptr, &rerr);
        if (rerr) {
          err = rerr->message ? rerr->message : "Download failed";
          g_error_free(rerr);
          ok = false;
          break;
        }
        if (r <= 0) break;
        if (fwrite(buf, 1, static_cast<size_t>(r), f) !=
            static_cast<size_t>(r)) {
          err = "Cannot write file";
          ok = false;
          break;
        }
        if (prog) prog->done_bytes.fetch_add(static_cast<uint64_t>(r));
      }
      fclose(f);
      if (!ok) std::remove(local_path.c_str()); // no partial files
    }
  }
  if (gerr) g_error_free(gerr);
  if (in) g_object_unref(in);
  g_object_unref(msg);
  g_object_unref(session);
  return ok;
}

#endif // EH_HAVE_DRIVE


#ifdef EH_HAVE_DRIVE

namespace {

struct DriveTask {
  bool src_drive = false;
  bool dst_drive = false;
  std::string src;
  std::string dst;
  std::string top; // top-level source (overwrite matching, move cleanup)
  std::string src_id; // resolved Drive id (drive sources)
  std::string src_mime; // Drive mime (drive sources; picks export)
  std::string src_did; // source corpus ("" = My Drive)
  std::string dst_did; // dest corpus ("" = My Drive)
  bool is_dir = false;
  uint64_t size = 0;
};

// Corpus for a display path, structurally (no walk): "" for My Drive,
// the shared-drive id under "/Shared drives/<name>/…". Matches names
// through the id cache, listing only on a miss.
std::string drive_corpus_for(AppState& app, const std::string& email,
                             const std::string& access,
                             const std::string& dpath, std::string& err) {
  auto segs = split_raw(dpath);
  if (segs.size() < 2 || unescape_seg(segs[0]) != kSharedPseudo) return "";
  {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (DriveAccount* acc = drive_find(app, email)) {
      auto it = acc->id_cache.find("/Shared drives/" + segs[1]);
      if (it != acc->id_cache.end())
        return drive_real_id(it->second);
    }
  }
  std::vector<SharedDrive> drives;
  if (!drive_list_shared_drives(access, drives, err)) return "";
  std::string want = unescape_seg(segs[1]);
  std::string did;
  for (auto& d : drives) {
    std::lock_guard<std::mutex> lk(app.scan_mtx);
    if (DriveAccount* acc = drive_find(app, email))
      acc->id_cache["/Shared drives/" + escape_seg(d.name)] =
          "shared:" + d.id;
    if (d.name == want) did = d.id;
  }
  return did;
}

struct DriveOpCtx {
  AppState* ap = nullptr;
  std::map<std::string, std::string> tokens; // email -> access
  std::string err;
  bool access(const std::string& email, std::string& out) {
    auto it = tokens.find(email);
    if (it != tokens.end()) {
      out = it->second;
      return true;
    }
    if (!drive_ensure_token(*ap, email, out, err)) return false;
    tokens[email] = out;
    return true;
  }
};

// Join a native rel path onto a Drive folder URI (escapes segments).
std::string drive_join(const std::string& base_uri,
                       const std::string& rel_native) {
  std::string out = base_uri;
  size_t i = 0;
  while (i < rel_native.size()) {
    size_t j = rel_native.find('/', i);
    if (j == std::string::npos) j = rel_native.size();
    if (j > i) {
      if (!out.empty() && out.back() != '/') out += '/';
      out += escape_seg(rel_native.substr(i, j - i));
    }
    i = j + 1;
  }
  return out;
}

bool flatten_drive_dir(DriveOpCtx& ctx, const std::string& email,
                       const std::string& folder_id, const std::string& did,
                       const std::string& src_uri, const std::string& dst,
                       bool dst_is_drive, const std::string& dst_did,
                       const std::string& top,
                       const std::function<bool()>& cancel,
                       std::vector<DriveTask>& tasks, uint64_t& bytes) {
  std::string access;
  if (!ctx.access(email, access)) return false;
  std::vector<DriveItem> kids;
  std::string lerr;
  if (!drive_list_children(access, drive_real_id(folder_id), kids, lerr,
                           cancel, did)) {
    if (!lerr.empty()) ctx.err = lerr;
    return false;
  }
  for (auto& k : kids) {
    if (cancel()) return false;
    std::string cs = src_uri + "/" + escape_seg(k.name);
    std::string cd = dst_is_drive
                         ? dst + "/" + escape_seg(k.name)
                         : (fs::path(dst) / k.name).string();
    if (k.is_folder) {
      if (!flatten_drive_dir(ctx, email, k.id, did, cs, cd, dst_is_drive,
                             dst_did, top, cancel, tasks, bytes))
        return false;
    } else {
      DriveTask t;
      t.src_drive = true;
      t.dst_drive = dst_is_drive;
      t.src = cs;
      t.dst = cd;
      t.top = top;
      t.src_id = k.id;
      t.src_mime = k.mime;
      t.src_did = did;
      t.dst_did = dst_did;
      t.size = k.size;
      tasks.push_back(std::move(t));
      bytes += k.size;
    }
  }
  return true;
}

bool flatten_local_dir(const std::string& src, const std::string& dst,
                       bool dst_is_drive, const std::string& top,
                       std::vector<DriveTask>& tasks, uint64_t& bytes,
                       std::string& err) {
  std::error_code ec;
  if (fs::is_directory(src, ec)) {
    for (auto& de : fs::recursive_directory_iterator(src, ec)) {
      if (!de.is_regular_file(ec)) continue;
      std::error_code ec2;
      std::string rel = fs::relative(de.path(), src, ec2).string();
      if (ec2) continue;
      uint64_t sz = de.file_size(ec2);
      if (ec2) sz = 0;
      DriveTask t;
      t.src_drive = false;
      t.dst_drive = dst_is_drive;
      t.src = de.path().string();
      t.dst = dst_is_drive ? drive_join(dst, rel)
                           : (fs::path(dst) / rel).string();
      t.top = top;
      t.size = sz;
      tasks.push_back(std::move(t));
      bytes += sz;
    }
    return true;
  }
  if (fs::exists(src, ec)) {
    uint64_t sz = fs::file_size(src, ec);
    if (ec) sz = 0;
    DriveTask t;
    t.src_drive = false;
    t.dst_drive = dst_is_drive;
    t.src = src;
    t.dst = dst;
    t.top = top;
    t.size = sz;
    tasks.push_back(std::move(t));
    bytes += sz;
    return true;
  }
  err = "Source not found";
  return false;
}

std::string drive_upload_mime(const std::string& local_path) {
  std::string m = mime_by_ext(local_path);
  return m.empty() ? "application/octet-stream" : m;
}

// mkdir -p for a local path (no-op when it already exists).
bool local_mkdir_p(const std::string& dir, std::string& err) {
  if (dir.empty()) return true;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    err = "Cannot create folder";
    return false;
  }
  return true;
}

} // namespace

void drive_do_copy_move(std::vector<std::string> src_paths,
                        std::string dest_dir, bool is_move,
                        std::shared_ptr<struct OperationProgress> prog,
                        std::function<void(bool cancelled)> on_complete,
                        std::vector<std::string> allow_overwrite,
                        std::vector<std::string> dst_names,
                        std::shared_ptr<std::string> error_out) {
  AppState* ap = drive_bound_app();
  if (!ap) {
    if (error_out && error_out->empty())
      *error_out = "Drive unavailable";
    if (prog) prog->success.store(false);
    DeferredCall::callLater([on_complete] {
      if (on_complete) on_complete(false);
    });
    return;
  }
  std::thread([ap, src_paths = std::move(src_paths),
               dest_dir = std::move(dest_dir), is_move, prog,
               on_complete = std::move(on_complete),
               allow_overwrite = std::move(allow_overwrite),
               dst_names = std::move(dst_names),
               error_out = std::move(error_out)] {
    prog->start_time = std::chrono::steady_clock::now();
    auto cancelled = [&] { return prog->cancel.load(); };
    auto fail = [&](const std::string& msg) {
      prog->success.store(false);
      if (error_out && error_out->empty()) *error_out = msg;
    };
    auto finish = [&](bool was_cancelled) {
      prog->active.store(false);
      prog->clear_current_file();
      DeferredCall::callLater([on_complete, was_cancelled] {
        if (on_complete) on_complete(was_cancelled);
      });
    };

    DriveOpCtx ctx;
    ctx.ap = ap;
    std::vector<DriveTask> tasks;
    uint64_t total_bytes = 0;
    struct TopMove {
      std::string src, dst;
    };
    std::vector<TopMove> fast_moves; // same-account server-side moves
    std::vector<std::string> move_cleanup; // tops to remove after success

    for (size_t si = 0; si < src_paths.size() && !cancelled(); ++si) {
      std::string fname = fs::path(src_paths[si]).filename().string();
      if (si < dst_names.size() && !dst_names[si].empty())
        fname = dst_names[si];
      std::string dest_base = dest_dir;
      if (!dest_base.empty() && dest_base.back() != '/') dest_base += '/';
      bool s_drive = is_drive_uri(src_paths[si]);
      bool d_drive = is_drive_uri(dest_base);
      // Same-account server-side move: one call, children ride along.
      // (The "/Shared drives" pseudo-root itself can't move — copy it.)
      if (is_move && s_drive && d_drive &&
          drive_display_path(src_paths[si]) != "/Shared drives" &&
          drive_email(src_paths[si]) ==
              drive_email(dest_base + fname)) {
        DriveStat st;
        std::string serr;
        if (!drive_stat_uri(src_paths[si], st, serr) || !st.exists) {
          fail("Source not found");
          break;
        }
        // mkdir -p the destination parent first.
        std::string email = drive_email(src_paths[si]);
        std::string access;
        if (!ctx.access(email, access)) {
          fail(ctx.err.empty() ? "Drive sign-in needed" : ctx.err);
          break;
        }
        std::string dst_parent = drive_parent(dest_base + fname);
        std::string leaf, dst_did_ignored;
        if (!drive_ensure_path(*ap, email, access,
                               drive_display_path(dst_parent), leaf,
                               dst_did_ignored, ctx.err,
                               [&] { return cancelled(); })) {
          fail(ctx.err.empty() ? "Cannot create folder" : ctx.err);
          break;
        }
        std::string merr;
        if (!drive_move_path(src_paths[si], dest_base + fname, merr)) {
          fail(merr.empty() ? "Move failed" : merr);
          break;
        }
        prog->total_files.fetch_add(1);
        prog->copied_files.fetch_add(1);
        prog->total_bytes.fetch_add(st.size);
        prog->done_bytes.fetch_add(st.size);
        continue;
      }
      if (s_drive) {
        std::string email = drive_email(src_paths[si]);
        DriveStat st;
        std::string serr;
        if (!drive_stat_uri(src_paths[si], st, serr) || !st.exists) {
          fail("Source not found");
          break;
        }
        if (st.is_dir) {
          std::string access;
          if (!ctx.access(email, access)) {
            fail(ctx.err.empty() ? "Drive sign-in needed" : ctx.err);
            break;
          }
          DriveLoc sloc = drive_locate(
              *ap, email, drive_display_path(src_paths[si]), ctx.err,
              [&] { return cancelled(); });
          if (sloc.id.empty()) {
            if (ctx.err.empty()) fail("Source not found");
            else fail(ctx.err);
            break;
          }
          std::string dst_did = d_drive
                                    ? drive_corpus_for(*ap, email, access,
                                                       drive_display_path(
                                                           dest_base + fname),
                                                       ctx.err)
                                    : "";
          if (!ctx.err.empty()) {
            fail(ctx.err);
            break;
          }
          if (sloc.id == "shared:roots") {
            // Copying the pseudo-root expands to every shared drive.
            std::vector<SharedDrive> drives;
            if (!drive_list_shared_drives(access, drives, ctx.err)) {
              fail(ctx.err.empty() ? "Cannot list shared drives" : ctx.err);
              break;
            }
            for (auto& d : drives) {
              if (cancelled()) break;
              std::string cs =
                  src_paths[si] + "/" + escape_seg(d.name);
              std::string cd = (dest_base + fname) + "/" + escape_seg(d.name);
              if (!flatten_drive_dir(ctx, email, d.id, d.id, cs, cd, d_drive,
                                     dst_did, src_paths[si],
                                     [&] { return cancelled(); }, tasks,
                                     total_bytes)) {
                if (!ctx.err.empty()) fail(ctx.err);
                break;
              }
            }
            if (!ctx.err.empty() || cancelled()) break;
          } else if (!flatten_drive_dir(ctx, email, sloc.id, sloc.drive_id,
                                        src_paths[si], dest_base + fname,
                                        d_drive, dst_did, src_paths[si],
                                        [&] { return cancelled(); }, tasks,
                                        total_bytes)) {
            if (!ctx.err.empty()) fail(ctx.err);
            break;
          }
        } else {
          // Single file: locate once for id+corpus, then read the exact
          // child record (mime/size) from the parent listing.
          std::string access;
          if (!ctx.access(email, access)) {
            fail(ctx.err.empty() ? "Drive sign-in needed" : ctx.err);
            break;
          }
          std::string dpath = drive_display_path(src_paths[si]);
          DriveLoc sloc =
              drive_locate(*ap, email, dpath, ctx.err, [&] { return cancelled(); });
          if (sloc.id.empty() || sloc.id == "shared:roots" ||
              sloc.id.rfind("shared:", 0) == 0) {
            fail(ctx.err.empty() ? "Source not found" : ctx.err);
            break;
          }
          std::string parent_raw = dpath;
          auto slash = parent_raw.rfind('/');
          parent_raw = (slash == std::string::npos || slash == 0)
                           ? ""
                           : parent_raw.substr(0, slash);
          DriveLoc ploc =
              drive_locate(*ap, email, parent_raw, ctx.err, [&] { return cancelled(); });
          if (ploc.id.empty()) {
            fail(ctx.err.empty() ? "Source not found" : ctx.err);
            break;
          }
          std::vector<DriveItem> kids;
          if (!drive_list_children(access, drive_real_id(ploc.id), kids,
                                   ctx.err, [&] { return cancelled(); },
                                   ploc.drive_id)) {
            fail(ctx.err.empty() ? "Source not found" : ctx.err);
            break;
          }
          std::string want = unescape_seg(dpath.substr(
              dpath.rfind('/') == std::string::npos ? 0
                                                    : dpath.rfind('/') + 1));
          bool hit = false;
          bool corpus_broken = false;
          for (auto& k : kids) {
            if (k.name != want) continue;
            std::string dst_did = d_drive
                                      ? drive_corpus_for(
                                            *ap, email, access,
                                            drive_display_path(dest_base +
                                                               fname),
                                            ctx.err)
                                      : "";
            if (!ctx.err.empty()) {
              fail(ctx.err);
              corpus_broken = true;
              break;
            }
            DriveTask t;
            t.src_drive = true;
            t.dst_drive = d_drive;
            t.src = src_paths[si];
            t.dst = dest_base + fname;
            t.top = src_paths[si];
            t.src_id = k.id;
            t.src_mime = k.mime;
            t.src_did = sloc.drive_id;
            t.dst_did = dst_did;
            t.size = k.size;
            tasks.push_back(std::move(t));
            total_bytes += k.size;
            hit = true;
            break;
          }
          if (!hit && !corpus_broken) {
            fail("Source not found");
            break;
          }
          if (corpus_broken) break;
        }
      } else {
        std::string ferr;
        if (!flatten_local_dir(src_paths[si], dest_base + fname, d_drive,
                               src_paths[si], tasks, total_bytes, ferr)) {
          fail(ferr);
          break;
        }
      }
      if (is_move) move_cleanup.push_back(src_paths[si]);
    }
    prog->total_files.store(static_cast<int>(tasks.size()));
    prog->total_bytes.store(total_bytes);

    auto dest_exists = [&](const std::string& dst) {
      if (is_drive_uri(dst)) {
        DriveStat st;
        std::string e;
        return drive_stat_uri(dst, st, e) && st.exists;
      }
      std::error_code ec;
      return fs::exists(dst, ec);
    };
    auto remove_dest = [&](const std::string& dst) {
      if (is_drive_uri(dst)) {
        std::string e;
        return drive_delete_path(dst, e);
      }
      std::error_code ec;
      fs::remove_all(dst, ec);
      return !ec;
    };

    int done = 0;
    for (auto& t : tasks) {
      if (cancelled()) break;
      bool allowed = std::find(allow_overwrite.begin(), allow_overwrite.end(),
                               t.top) != allow_overwrite.end();
      if (dest_exists(t.dst)) {
        if (!allowed) {
          fail("Destination exists");
          break;
        }
        if (!remove_dest(t.dst)) {
          fail("Cannot replace existing file");
          break;
        }
      }
      {
        std::string base = fs::path(t.src).filename().string();
        if (t.src_drive) {
          auto slash = t.src.rfind('/');
          base = (slash == std::string::npos) ? t.src
                                              : t.src.substr(slash + 1);
          gchar* u = g_uri_unescape_string(base.c_str(), nullptr);
          if (u) {
            base = u;
            g_free(u);
          }
        }
        prog->set_current_file(base);
      }
      bool ok = false;
      std::string err;
      if (!t.src_drive && !t.dst_drive) {
        std::error_code ec;
        if (!t.is_dir) {
          local_mkdir_p(fs::path(t.dst).parent_path().string(), err);
          ok = err.empty() &&
               fs::copy_file(t.src, t.dst,
                             fs::copy_options::overwrite_existing, ec) &&
               !ec;
          if (!ok && err.empty()) err = "Copy failed";
        } else {
          ok = local_mkdir_p(t.dst, err);
        }
      } else if (!t.src_drive && t.dst_drive) {
        std::string email = drive_email(t.dst);
        std::string access;
        if (!ctx.access(email, access)) {
          fail(ctx.err.empty() ? "Drive sign-in needed" : ctx.err);
          break;
        }
        std::string parent_raw = drive_display_path(t.dst);
        auto slash = parent_raw.rfind('/');
        std::string name_raw = (slash == std::string::npos)
                                   ? parent_raw
                                   : parent_raw.substr(slash + 1);
        parent_raw = (slash == std::string::npos || slash == 0)
                         ? ""
                         : parent_raw.substr(0, slash);
        std::string parent_id, parent_did;
        if (!drive_ensure_path(*ap, email, access, parent_raw, parent_id,
                               parent_did, ctx.err,
                               [&] { return cancelled(); })) {
          fail(ctx.err.empty() ? "Cannot create folder" : ctx.err);
          break;
        }
        if (t.is_dir) {
          std::string id;
          ok = drive_api_mkdir(access, parent_id, unescape_seg(name_raw), id,
                               err, parent_did);
        } else {
          std::string id;
          ok = drive_upload_file(access, parent_id, unescape_seg(name_raw),
                                 drive_upload_mime(t.src), t.src, prog.get(),
                                 [&] { return cancelled(); }, id, err,
                                 parent_did);
        }
        if (!ok && err.empty()) err = "Upload failed";
      } else if (t.src_drive && !t.dst_drive) {
        std::string email = drive_email(t.src);
        std::string access;
        if (!ctx.access(email, access)) {
          fail(ctx.err.empty() ? "Drive sign-in needed" : ctx.err);
          break;
        }
        if (t.is_dir) {
          ok = local_mkdir_p(t.dst, err);
        } else {
          local_mkdir_p(fs::path(t.dst).parent_path().string(), err);
          if (!err.empty()) {
            fail(err);
            break;
          }
          const GoogleKind* gk = google_kind(t.src_mime);
          std::string export_mime =
              (gk && gk->export_mime) ? gk->export_mime : "";
          std::string dst = t.dst;
          if (!export_mime.empty()) {
            std::string suf = gk->export_suffix;
            std::string lower = dst;
            for (auto& c : lower)
              c = static_cast<char>(
                  std::tolower(static_cast<unsigned char>(c)));
            if (lower.size() < suf.size() ||
                lower.compare(lower.size() - suf.size(), suf.size(), suf) !=
                    0)
              dst += suf;
          }
          ok = drive_download_to(access, t.src_id, export_mime, dst,
                                 prog.get(), [&] { return cancelled(); }, err,
                                 t.src_did);
        }
        if (!ok && err.empty()) err = "Download failed";
      } else {
        std::string src_email = drive_email(t.src);
        std::string dst_email = drive_email(t.dst);
        std::string s_access, d_access;
        if (!ctx.access(src_email, s_access) ||
            !ctx.access(dst_email, d_access)) {
          fail(ctx.err.empty() ? "Drive sign-in needed" : ctx.err);
          break;
        }
        std::string dst_parent_raw = drive_display_path(t.dst);
        auto slash = dst_parent_raw.rfind('/');
        std::string dst_name_raw =
            (slash == std::string::npos)
                ? dst_parent_raw
                : dst_parent_raw.substr(slash + 1);
        dst_parent_raw = (slash == std::string::npos || slash == 0)
                             ? ""
                             : dst_parent_raw.substr(0, slash);
        std::string dst_parent_id, dst_parent_did;
        if (!drive_ensure_path(*ap, dst_email, d_access, dst_parent_raw,
                               dst_parent_id, dst_parent_did, ctx.err,
                               [&] { return cancelled(); })) {
          fail(ctx.err.empty() ? "Cannot create folder" : ctx.err);
          break;
        }
        if (t.is_dir) {
          std::string id;
          ok = drive_api_mkdir(d_access, dst_parent_id,
                               unescape_seg(dst_name_raw), id, err,
                               dst_parent_did);
          if (!ok && err.empty()) err = "Cannot create folder";
        } else if (src_email == dst_email && t.src_did == t.dst_did) {
          // Server-side copy: no bytes move.
          std::string id;
          ok = drive_api_copy(s_access, t.src_id, dst_parent_id,
                              unescape_seg(dst_name_raw), id, err,
                              dst_parent_did);
          if (!ok && err.empty()) err = "Copy failed";
          else if (ok) {
            prog->done_bytes.fetch_add(t.size);
          }
        } else {
          // Cross-account (or cross-corpus) relay through a temp file.
          char tmpl[] = "/tmp/horizon-drive-relay-XXXXXX";
          int fd = mkstemp(tmpl);
          if (fd < 0) {
            fail("Cannot stage download");
            break;
          }
          ::close(fd);
          std::string tmp = tmpl;
          const GoogleKind* gk = google_kind(t.src_mime);
          std::string export_mime =
              (gk && gk->export_mime) ? gk->export_mime : "";
          ok = drive_download_to(s_access, t.src_id, export_mime, tmp,
                                 prog.get(), [&] { return cancelled(); }, err,
                                 t.src_did);
          if (ok) {
            std::string id;
            ok = drive_upload_file(
                d_access, dst_parent_id, unescape_seg(dst_name_raw),
                drive_upload_mime(tmp), tmp, prog.get(),
                [&] { return cancelled(); }, id, err, dst_parent_did);
          }
          std::remove(tmp.c_str());
          if (!ok && err.empty()) err = "Copy failed";
        }
      }
      if (!ok) {
        if (err.empty()) {
          if (cancelled()) break;
          err = "Copy failed";
        }
        fail(err);
        break;
      }
      prog->done_bytes.fetch_add(0); // transfers already accounted
      prog->copied_files.fetch_add(1);
      ++done;
      (void)done;
    }

    // Move cleanup: remove tops only when everything succeeded.
    if (is_move && !cancelled() && prog->success.load()) {
      for (auto& top : move_cleanup) {
        if (is_drive_uri(top)) {
          std::string e;
          if (!drive_delete_path(top, e)) {
            fail(e.empty() ? "Move cleanup failed" : e);
            break;
          }
        } else {
          std::error_code ec;
          fs::remove_all(top, ec);
          if (ec) {
            fail("Move cleanup failed");
            break;
          }
        }
      }
    }
    finish(cancelled() || prog->cancel.load());
  }).detach();
}

#else // !EH_HAVE_DRIVE

void drive_do_copy_move(std::vector<std::string> src_paths,
                        std::string dest_dir, bool is_move,
                        std::shared_ptr<struct OperationProgress> prog,
                        std::function<void(bool cancelled)> on_complete,
                        std::vector<std::string> allow_overwrite,
                        std::vector<std::string> dst_names,
                        std::shared_ptr<std::string> error_out) {
  (void)src_paths;
  (void)dest_dir;
  (void)is_move;
  (void)allow_overwrite;
  (void)dst_names;
  if (error_out && error_out->empty())
    *error_out = "Drive support not built";
  if (prog) prog->success.store(false);
  DeferredCall::callLater([on_complete] {
    if (on_complete) on_complete(false);
  });
}

#endif // EH_HAVE_DRIVE


#ifdef EH_HAVE_DRIVE

namespace {
int64_t drive_parse_time(JsonObject* obj, const char* key) {
  if (!obj || !json_object_has_member(obj, key)) return 0;
  GDateTime* dt = g_date_time_new_from_iso8601(
      json_object_get_string_member(obj, key), nullptr);
  if (!dt) return 0;
  int64_t t = g_date_time_to_unix(dt);
  g_date_time_unref(dt);
  return t;
}
} // namespace

bool drive_file_meta(const std::string& access_token, const std::string& id,
                     const std::string& drive_id, DriveMeta& out,
                     std::string& err) {
  out = DriveMeta{};
  std::string url = std::string(kFilesEndpoint) + "/" + id +
                    "?fields=" +
                    form_escape("id,mimeType,size,modifiedTime,createdTime,"
                                "viewedByMeTime,owners(displayName,"
                                "emailAddress),shared,version,md5Checksum,"
                                "trashed");
  drive_sup(url, drive_id, false);
  int status = 0;
  std::string resp = soup_fetch(url, access_token, {}, status, err);
  if (resp.empty() || (status < 200 || status >= 300)) {
    if (err.empty()) err = "Drive lookup failed";
    return false;
  }
  JsonParser* parser = json_parser_new();
  GError* gerr = nullptr;
  bool ok = false;
  if (json_parser_load_from_data(parser, resp.c_str(),
                                 static_cast<gssize>(resp.size()), &gerr)) {
    JsonNode* root = json_parser_get_root(parser);
    JsonObject* obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                          ? json_node_get_object(root)
                          : nullptr;
    if (obj) {
      bool trashed = json_object_has_member(obj, "trashed") &&
                     json_object_get_boolean_member(obj, "trashed");
      if (!trashed) {
        out.exists = true;
        const char* mime = json_object_has_member(obj, "mimeType")
                               ? json_object_get_string_member(obj, "mimeType")
                               : "";
        out.mime = mime ? mime : "";
        out.is_dir = out.mime == kFolderMime;
        if (!out.is_dir && json_object_has_member(obj, "size")) {
          try {
            out.size =
                std::stoull(json_object_get_string_member(obj, "size"));
          } catch (...) {
          }
        }
        out.mtime = drive_parse_time(obj, "modifiedTime");
        out.ctime = drive_parse_time(obj, "createdTime");
        out.viewed = drive_parse_time(obj, "viewedByMeTime");
        out.shared = json_object_has_member(obj, "shared") &&
                     json_object_get_boolean_member(obj, "shared");
        if (json_object_has_member(obj, "version"))
          out.version = json_object_get_string_member(obj, "version");
        if (json_object_has_member(obj, "md5Checksum"))
          out.md5 = json_object_get_string_member(obj, "md5Checksum");
        if (json_object_has_member(obj, "owners")) {
          JsonArray* owners = json_object_get_array_member(obj, "owners");
          if (owners && json_array_get_length(owners) > 0) {
            JsonObject* o = json_array_get_object_element(owners, 0);
            if (o) {
              if (json_object_has_member(o, "displayName"))
                out.owner = json_object_get_string_member(o, "displayName");
              if (json_object_has_member(o, "emailAddress"))
                out.owner_email =
                    json_object_get_string_member(o, "emailAddress");
            }
          }
        }
        ok = true;
      }
    } else {
      err = "bad Drive response";
    }
  } else {
    err = gerr && gerr->message ? gerr->message : "bad Drive response";
    if (gerr) g_error_free(gerr);
  }
  g_object_unref(parser);
  return ok;
}

bool drive_about_quota(const std::string& access_token, uint64_t& used_out,
                       uint64_t& total_out, std::string& err) {
  used_out = 0;
  total_out = 0;
  std::string url =
      "https://www.googleapis.com/drive/v3/about?fields=" +
      form_escape("storageQuota(limit,usage)");
  int status = 0;
  std::string resp = soup_fetch(url, access_token, {}, status, err);
  if (resp.empty() || (status < 200 || status >= 300)) {
    if (err.empty()) err = "Drive quota lookup failed";
    return false;
  }
  JsonParser* parser = json_parser_new();
  GError* gerr = nullptr;
  bool ok = false;
  if (json_parser_load_from_data(parser, resp.c_str(),
                                 static_cast<gssize>(resp.size()), &gerr)) {
    JsonNode* root = json_parser_get_root(parser);
    JsonObject* obj = (root && JSON_NODE_HOLDS_OBJECT(root))
                          ? json_node_get_object(root)
                          : nullptr;
    JsonObject* q = obj && json_object_has_member(obj, "storageQuota")
                        ? json_object_get_object_member(obj, "storageQuota")
                        : nullptr;
    if (q) {
      try {
        if (json_object_has_member(q, "usage"))
          used_out =
              std::stoull(json_object_get_string_member(q, "usage"));
        if (json_object_has_member(q, "limit"))
          total_out =
              std::stoull(json_object_get_string_member(q, "limit"));
      } catch (...) {
      }
      ok = true;
    } else {
      err = "bad Drive response";
    }
  } else {
    err = gerr && gerr->message ? gerr->message : "bad Drive response";
    if (gerr) g_error_free(gerr);
  }
  g_object_unref(parser);
  return ok;
}

bool drive_walk_size(const std::string& access_token,
                     const std::string& folder_id, const std::string& drive_id,
                     DriveTreeSize& out, std::string& err,
                     const std::function<bool()>& gen_done) {
  std::vector<std::string> stack{drive_real_id(folder_id)};
  while (!stack.empty()) {
    if (gen_done && gen_done()) {
      err.clear();
      return false;
    }
    std::string cur = std::move(stack.back());
    stack.pop_back();
    std::vector<DriveItem> kids;
    std::string lerr;
    if (!drive_list_children(access_token, cur, kids, lerr, gen_done,
                             drive_id)) {
      if (!lerr.empty()) {
        err = lerr;
        return false;
      }
      return false; // superseded: stay silent
    }
    for (auto& k : kids) {
      if (k.is_folder) {
        ++out.dirs;
        stack.push_back(k.id);
      } else {
        ++out.files;
        out.bytes += k.size;
      }
    }
  }
  return true;
}

#else // !EH_HAVE_DRIVE

bool drive_file_meta(const std::string&, const std::string&,
                     const std::string&, DriveMeta&, std::string& err) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}
bool drive_about_quota(const std::string&, uint64_t&, uint64_t&,
                       std::string& err) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}
bool drive_walk_size(const std::string&, const std::string&,
                     const std::string&, DriveTreeSize&, std::string& err,
                     const std::function<bool()>&) {
  err = "Drive support not built (missing libsoup/json-glib)";
  return false;
}

#endif // EH_HAVE_DRIVE

bool drive_folder_size(const std::string& uri, uint64_t& files_out,
                       uint64_t& dirs_out, uint64_t& bytes_out,
                       std::string& err) {
  files_out = dirs_out = bytes_out = 0;
  if (!is_drive_uri(uri)) {
    err = "not a Drive location";
    return false;
  }
#ifdef EH_HAVE_DRIVE
  AppState* ap = nullptr;
  std::string access;
  std::string email = drive_email(uri);
  if (!drive_path_creds(email, ap, access, err)) return false;
  DriveLoc loc = drive_locate(*ap, email, drive_display_path(uri), err,
                              [] { return false; });
  if (loc.id.empty() || loc.id == "shared:roots") {
    if (err.empty()) err = "Not found in Drive";
    return false;
  }
  DriveTreeSize tree;
  if (!drive_walk_size(access, drive_real_id(loc.id), loc.drive_id, tree, err,
                       [] { return false; }))
    return false;
  files_out = tree.files;
  dirs_out = tree.dirs;
  bytes_out = tree.bytes;
  return true;
#else
  (void)err;
  err = "Drive support not built";
  return false;
#endif
}

const char* drive_kind_label(const std::string& mime) {  if (mime.rfind(kGooglePrefix, 0) != 0) return nullptr;
  std::string tail =
      mime.substr(std::char_traits<char>::length(kGooglePrefix));
  if (tail == "document") return "Google Docs document";
  if (tail == "spreadsheet") return "Google Sheets spreadsheet";
  if (tail == "presentation") return "Google Slides presentation";
  if (tail == "drawing") return "Google Drawing";
  if (tail == "form") return "Google Form";
  if (tail == "site") return "Google Site";
  if (tail == "map") return "Google My Map";
  if (tail == "jam") return "Jamboard whiteboard";
  if (tail == "script") return "Apps Script project";
  if (tail == "audio") return "Google Drive audio";
  if (tail == "video" || tail == "vid") return "Google Drive video";
  if (tail == "photo" || tail == "pic") return "Google photo";
  if (tail == "fusiontable") return "Fusion Table";
  if (tail == "mail-layout") return "Gmail layout";
  return "Google Drive file";
}

std::string drive_export_target(const std::string& mime,
                                std::string& suffix_out) {
  suffix_out.clear();
  if (const GoogleKind* g = google_kind(mime)) {
    if (g->export_mime) {
      suffix_out = g->export_suffix;
      return g->export_mime;
    }
  }
  return {};
}

#ifdef EH_HAVE_KEYRING
#include <libsecret/secret.h>

namespace {

SecretSchema* drive_schema() {
  static SecretSchema* s = secret_schema_new(
      "org.horizon.DriveAccount", SECRET_SCHEMA_DONT_MATCH_NAME, "email",
      SECRET_SCHEMA_ATTRIBUTE_STRING, "kind", SECRET_SCHEMA_ATTRIBUTE_STRING,
      nullptr);
  return s;
}
constexpr const char* kDriveKind = "horizon-drive-account";

std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

} // namespace
#endif

bool drive_keyring_available() {
#ifdef EH_HAVE_KEYRING
  return true;
#else
  return false;
#endif
}

void drive_keyring_store(const std::string& email,
                         const std::string& client_id,
                         const std::string& client_secret,
                         const std::string& refresh_token) {
#ifdef EH_HAVE_KEYRING
  if (email.empty() || refresh_token.empty()) return;
  std::string blob = "{\"client_id\":\"" + json_escape(client_id) +
                     "\",\"client_secret\":\"" + json_escape(client_secret) +
                     "\",\"refresh_token\":\"" + json_escape(refresh_token) +
                     "\"}";
  std::string label = "Horizon Files Google Drive (" + email + ")";
  GError* err = nullptr;
  secret_password_store_sync(drive_schema(), SECRET_COLLECTION_DEFAULT,
                             label.c_str(), blob.c_str(), nullptr, &err,
                             "email", email.c_str(), "kind", kDriveKind,
                             nullptr);
  if (err) {
    fprintf(stderr, "horizon-files: drive keyring store failed: %s\n",
            err->message ? err->message : "unknown");
    g_error_free(err);
  }
#else
  (void)email;
  (void)client_id;
  (void)client_secret;
  (void)refresh_token;
#endif
}

bool drive_keyring_lookup(const std::string& email, std::string& client_id_out,
                          std::string& secret_out, std::string& refresh_out) {
#ifdef EH_HAVE_KEYRING
  if (email.empty()) return false;
  GError* err = nullptr;
  gchar* secret = secret_password_lookup_sync(
      drive_schema(), nullptr, &err, "email", email.c_str(), "kind",
      kDriveKind, nullptr);
  if (err) {
    g_error_free(err);
    return false;
  }
  if (!secret) return false;
  std::string blob = secret;
  secret_password_free(secret);
  // Minimal JSON read (writer above only emits these three string keys).
  auto grab = [&](const char* key, std::string& dst) {
    std::string k = std::string("\"") + key + "\":\"";
    auto pos = blob.find(k);
    if (pos == std::string::npos) return;
    pos += k.size();
    std::string val;
    while (pos < blob.size() && blob[pos] != '"') {
      if (blob[pos] == '\\' && pos + 1 < blob.size()) ++pos;
      val += blob[pos++];
    }
    dst = val;
  };
  grab("client_id", client_id_out);
  grab("client_secret", secret_out);
  grab("refresh_token", refresh_out);
  return !refresh_out.empty();
#else
  (void)email;
  (void)client_id_out;
  (void)secret_out;
  (void)refresh_out;
  return false;
#endif
}

void drive_keyring_erase(const std::string& email) {
#ifdef EH_HAVE_KEYRING
  if (email.empty()) return;
  GError* err = nullptr;
  secret_password_clear_sync(drive_schema(), nullptr, &err, "email",
                             email.c_str(), "kind", kDriveKind, nullptr);
  if (err) g_error_free(err);
#else
  (void)email;
#endif
}

void drive_restore_accounts(AppState& app) {
  eh::config::FileBrowserSettings fbs = eh::config::read_file_browser_toml();
  if (fbs.drive_account_emails.empty()) return;
  if (!drive_keyring_available()) return;
  AppState* ap = &app;
  std::vector<std::string> emails = fbs.drive_account_emails;
  std::thread([ap, emails]() {
    int revived = 0;
    for (auto& email : emails) {
      {
        std::lock_guard<std::mutex> lk(ap->scan_mtx);
        if (drive_find(*ap, email)) continue;
      }
      std::string cid, sec, ref;
      if (!drive_keyring_lookup(email, cid, sec, ref)) continue;
      std::string access;
      int64_t expires_in = 0;
      std::string err;
      if (!drive_refresh_token(cid, sec, ref, access, expires_in, err)) {
        // Revoked or rotated: forget so we don't retry every launch.
        drive_keyring_erase(email);
        continue;
      }
      {
        std::lock_guard<std::mutex> lk(ap->scan_mtx);
        if (drive_find(*ap, email)) continue;
        DriveAccount acc;
        acc.email = email;
        acc.refresh_token = ref;
        acc.access_token = access;
        acc.access_expiry_ms =
            now_ms() +
            (expires_in > 120 ? expires_in - 60 : expires_in) * 1000;
        acc.client_id = cid;
        acc.client_secret = sec;
        ap->drive_accounts.push_back(std::move(acc));
      }
      if (!cid.empty()) {
        std::lock_guard<std::mutex> lk(ap->scan_mtx);
        if (ap->drive_client_id.empty()) ap->drive_client_id = cid;
        if (ap->drive_client_secret.empty()) ap->drive_client_secret = sec;
      }
      ++revived;
    }
    if (revived > 0) {
      DeferredCall::callLater([ap, revived]() {
        refresh_sidebar(*ap);
        drive_toast(*ap, revived == 1 ? "Google Drive reconnected"
                                      : "Google Drive reconnected (" +
                                            std::to_string(revived) + ")");
      });
    }
  }).detach();
}

} // namespace eh::file_browser
