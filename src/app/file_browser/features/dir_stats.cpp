#include "dir_stats.hpp"

#include "../app_types.hpp"
#include "../trace.hpp"

#include <sys/stat.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace eh::file_browser {
namespace fs = std::filesystem;

namespace {

struct DirStatsResult {
  std::string path;
  uint64_t count = 0;
  uint64_t bytes = 0;
  bool truncated = false;
  int64_t mtime_sec = 0;
};

std::thread g_worker;
std::atomic<bool> g_running{false};
std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<std::string> g_queue;                       // ordered work
std::unordered_set<std::string> g_enqueued;            // dedup
std::deque<DirStatsResult> g_done;                     // finished, not yet drained

void walk(const std::string& path, DirStatsResult* out) {
  std::error_code ec;
  // Bounded walk: status bar numbers are informational; stop counting at a
  // million entries so pathological trees can't pin the worker forever.
  constexpr uint64_t kMaxEntries = 1'000'000;
  fs::recursive_directory_iterator it(
      path, fs::directory_options::skip_permission_denied, ec);
  if (ec) return;
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    if (ec) { out->truncated = true; ec.clear(); continue; }
    ++out->count;
    std::error_code fec;
    if (it->is_regular_file(fec) && !fec)
      out->bytes += static_cast<uint64_t>(it->file_size(fec));
    if (out->count >= kMaxEntries) {
      out->truncated = true;
      break;
    }
  }
  if (ec) out->truncated = true;
}

void worker_loop() {
  trace::set_thread_name("dir-stats");
  while (true) {
    std::string path;
    int64_t mtime = 0;
    {
      std::unique_lock<std::mutex> lk(g_mtx);
      g_cv.wait(lk, [] { return !g_running || !g_queue.empty(); });
      if (!g_running && g_queue.empty()) return;
      path = std::move(g_queue.front());
      g_queue.pop_front();
    }
    // Re-stat to capture the dir's mtime as of measurement
    struct ::stat st {};
    if (::stat(path.c_str(), &st) == 0) mtime = (int64_t)st.st_mtime;

    DirStatsResult r;
    r.path = path;
    r.mtime_sec = mtime;
    walk(path, &r);

    std::lock_guard<std::mutex> lk(g_mtx);
    g_enqueued.erase(path);
    g_done.emplace_back(std::move(r));
  }
}

}  // namespace

void dir_stats_start() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (g_running.exchange(true)) return;
  try {
    g_worker = std::thread(worker_loop);
  } catch (...) {
    g_running.store(false);
  }
}

void dir_stats_request(AppState& app, const std::string& path,
                       int64_t dir_mtime_sec) {
  if (path.empty()) return;
  auto it = app.dir_stat_cache.find(path);
  if (it != app.dir_stat_cache.end() && it->second.mtime_sec == dir_mtime_sec)
    return;  // fresh cached value

  {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_enqueued.count(path)) return;
    if (!g_running) return;
    g_enqueued.insert(path);
    g_queue.emplace_back(path);
  }
  g_cv.notify_one();
}

bool dir_stats_drain(AppState& app) {
  std::deque<DirStatsResult> done;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    done.swap(g_done);
  }
  if (done.empty()) return false;
  for (auto& r : done) {
    auto& e = app.dir_stat_cache[r.path];
    e.count = r.count;
    e.bytes = r.bytes;
    e.truncated = r.truncated;
    e.mtime_sec = r.mtime_sec;
  }
  return true;
}

void dir_stats_stop() {
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_running.exchange(false)) {
      if (g_worker.joinable()) g_worker.join();
      return;
    }
  }
  g_cv.notify_all();
  if (g_worker.joinable()) g_worker.join();
}

}  // namespace eh::file_browser
