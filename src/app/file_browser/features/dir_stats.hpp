#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace eh::file_browser {

struct AppState;

// Status-bar directory statistics ("N items (size)") computed OFF the UI
// thread. Painting only ever consults the ready cache; misses enqueue a
// background walk and the bar upgrades to full numbers when it lands.
//
//   dir_stats_request(app, path, dir_mtime)  — any thread (paint thread ok)
//   dir_stats_drain(app)                     — UI thread each loop iteration;
//                                              returns true if new results
//                                              arrived (caller redraws)
//   dir_stats_stop()                         — teardown, joins worker
//
// Results are cached per path with the directory's mtime at measurement
// time; paint re-requests when the mtime changed (content updated).

void dir_stats_start();
void dir_stats_request(AppState& app, const std::string& path,
                       int64_t dir_mtime_sec);
bool dir_stats_drain(AppState& app);
void dir_stats_stop();

} // namespace eh::file_browser
