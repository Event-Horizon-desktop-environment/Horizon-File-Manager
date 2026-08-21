#pragma once

#include <cairo/cairo.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/file_browser/app_types.hpp"

namespace eh::file_browser {

// Long-side pixel cap for full-res video preview frames (hover/space preview)
inline constexpr int kVideoPreviewFrameMaxPx = 800;

struct VideoThumbResult {
  std::string path;
  cairo_surface_t* surface{nullptr};
};

class VideoThumbWorker {
public:
  VideoThumbWorker();
  ~VideoThumbWorker();

  void enqueue(const std::string& path, int max_px,
               const std::string& cache_path, time_t src_mtime);
  void enqueue_preview(const std::string& path, int max_px);
  bool poll(VideoThumbResult& out);
  bool poll_preview(VideoThumbResult& out);
  bool busy();
  void stop();

private:
  struct WorkItem {
    std::string path;
    int max_px;
    std::string cache_path;
    time_t src_mtime;
    bool preview{false};   // full-res preview frame (not cached, not for thumb cache)
  };

  void thread_main(int thread_id);

  static constexpr int kNumThreads = 6;

  std::vector<std::thread> m_threads;
  std::mutex m_in_mutex;
  std::condition_variable m_in_cv;
  std::queue<WorkItem> m_in;
  std::set<std::string> m_pending;
  std::set<std::string> m_prev_pending;

  std::mutex m_out_mutex;
  std::queue<VideoThumbResult> m_out;
  std::queue<VideoThumbResult> m_prev_out;

  std::atomic<bool> m_running{true};
};

VideoThumbWorker& video_worker();

void drain_video_thumbnails(AppState& app);

} // namespace eh::file_browser
