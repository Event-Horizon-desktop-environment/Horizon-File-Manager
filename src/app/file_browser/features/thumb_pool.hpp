// Background thumbnail decoding pool — Dolphin/Nemo style.
//
// Iron rule: the UI thread never decodes. Paint only does cache lookups;
// a miss enqueues a request here and draws a placeholder. The single
// worker thread decodes (disk-cache load, image/svg/pdf/epub/video) and
// hands finished cairo surfaces back; the frame loop installs them into
// the shared thumb_cache between frames.
//
// One worker on purpose: keeps non-thread-safe decoders (poppler et al.)
// serialized, and 128–512 px thumbnails decode fast enough that a serial
// pipeline outpaces scrolling anyway.
#pragma once

#include <cairo/cairo.h>

#include <string>
#include <vector>

namespace eh::file_browser {

struct AppState;

struct ThumbBgResult {
  std::string path;
  int size = 0;
  cairo_surface_t* surface = nullptr;   // may be null (= decode failed)
};

// Start the worker. Safe to call once at startup.
void thumb_pool_start(class AppState* app);

// Stop the worker and join. Any pending requests are dropped.
void thumb_pool_stop();

// Queue a decode request. Deduplicates paths already queued/in-flight.
// UI thread only.
void thumb_pool_enqueue(AppState& app, const std::string& path, int size);

// Synchronous decode of one thumbnail (disk-cache aware, saves back to
// disk cache except for videos). Implemented in draw.cpp where the type
// loaders live. Thread-safe: touches no shared state.
cairo_surface_t* thumb_decode_sync(const std::string& path, int size,
                                   bool* used_video);

// Insert a finished surface into the app's thumb cache with LRU bookkeeping
// and eviction (UI thread only). Takes ownership of `s` either way.
void thumb_cache_install(AppState& app, const std::string& path, int size,
                         cairo_surface_t* s);

// Take ownership of finished results. UI thread only; surfaces must be
// installed (or destroyed) by the caller.
void thumb_pool_drain(AppState& app, std::vector<ThumbBgResult>& out);

}  // namespace eh::file_browser
