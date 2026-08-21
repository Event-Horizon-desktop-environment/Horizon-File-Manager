#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace eh::file_browser {

enum class OperationType : uint8_t {
  None,
  Copy,
  Move,
  Extract,
};

struct OperationProgress {
  std::atomic<bool> active{false};
  std::atomic<bool> cancel{false};
  std::atomic<bool> success{true};
  std::atomic<double> progress{0.0};
  std::atomic<int> total_files{0};
  std::atomic<int> copied_files{0};
  std::atomic<uint64_t> total_bytes{0};
  std::atomic<uint64_t> done_bytes{0};
  std::string current_file;
  OperationType type{OperationType::Copy};
  std::chrono::steady_clock::time_point start_time;

  double speed_mbps() const {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time).count();
    if (elapsed < 0.1) return 0.0;
    uint64_t done = done_bytes.load();
    return static_cast<double>(done) / (elapsed * 1024.0 * 1024.0);
  }

  double eta_seconds() const {
    double speed = speed_mbps();
    if (speed < 0.001) return -1.0;
    uint64_t total = total_bytes.load();
    uint64_t done = done_bytes.load();
    if (done >= total) return 0.0;
    double remaining_mb = static_cast<double>(total - done) / (1024.0 * 1024.0);
    return remaining_mb / speed;
  }
};

using OpCompleteCallback =
    std::function<void(bool cancelled)>;

void start_async_op(
    const std::vector<std::string>& src_paths,
    const std::string& dest_dir,
    bool is_move,
    std::shared_ptr<OperationProgress> prog,
    OpCompleteCallback on_complete = nullptr,
    const std::vector<std::string>& allow_overwrite = {},
    const std::vector<std::string>& dst_names = {});

} // namespace eh::file_browser
