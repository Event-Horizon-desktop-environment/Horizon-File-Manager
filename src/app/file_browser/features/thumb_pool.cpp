#include "thumb_pool.hpp"

#include "../app_types.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace eh::file_browser {

namespace {

std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<std::pair<std::string, int>> g_queue;
std::set<std::string> g_enqueued;
std::vector<ThumbBgResult> g_results;
AppState* g_app = nullptr;
bool g_running = false;
std::thread g_worker;

void worker_loop() {
  std::unique_lock<std::mutex> lk(g_mtx);
  for (;;) {
    while (g_queue.empty() && g_running) g_cv.wait(lk);
    if (!g_running && g_queue.empty()) return;
    auto [path, size] = std::move(g_queue.front());
    g_queue.pop_front();
    lk.unlock();

    // Pure decode — touches no AppState state.
    bool used_video = false;
    cairo_surface_t* s = thumb_decode_sync(path, size, &used_video);

    lk.lock();
    ThumbBgResult r;
    r.path = std::move(path);
    r.size = size;
    r.surface = s;
    g_results.push_back(std::move(r));
    g_enqueued.erase(r.path);
  }
}

}  // namespace

void thumb_pool_start(AppState* app) {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (g_running) return;
  g_app = app;
  g_running = true;
  g_worker = std::thread(worker_loop);
}

void thumb_pool_stop() {
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_running) return;
    g_running = false;
  }
  g_cv.notify_all();
  if (g_worker.joinable()) g_worker.join();
}

void thumb_pool_enqueue(AppState& app, const std::string& path, int size) {
  (void)app;
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_running || g_enqueued.count(path)) return;
  g_enqueued.insert(path);
  g_queue.emplace_back(path, size);
  g_cv.notify_one();
}

void thumb_pool_drain(AppState& app, std::vector<ThumbBgResult>& out) {
  std::lock_guard<std::mutex> lk(g_mtx);
  out.swap(g_results);
}

}  // namespace eh::file_browser
