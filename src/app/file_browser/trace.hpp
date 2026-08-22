// Lightweight thread-aware tracing for diagnosing the async scan pipeline.
// Enabled with EH_TRACE=1. Every line carries:
//   <ms since first use> <os-tid> <thread-name> <message>
// Thread names are set once per thread via set_thread_name() (15 char limit,
// visible in ps -T / gdb / perf). All writes funnel through one mutex'd
// stderr FILE* so lines never interleave mid-write.
#pragma once

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace eh::trace {

inline std::atomic<bool>& enabled() {
  static std::atomic<bool> v{[] {
    const char* e = std::getenv("EH_TRACE");
    return e && *e && e[0] != '0';
  }()};
  return v;
}

inline long tid() { return ::syscall(SYS_gettid); }

inline const char* thread_name() {
  static thread_local char name[16] = {0};
  if (name[0] == '\0') {
    if (::pthread_getname_np(::pthread_self(), name, sizeof(name)) != 0)
      std::snprintf(name, sizeof(name), "tid-%ld", tid());
  }
  return name;
}

// Name the current thread; must be called first thing on each worker.
// thread_name() picks it up via pthread_getname_np.
inline void set_thread_name(const char* n) {
  ::pthread_setname_np(::pthread_self(), n);
}

// Monotonic milliseconds since first trace use (process-lifetime anchor).
inline double now_ms() {
  using namespace std::chrono;
  static const auto base = steady_clock::now();
  return duration<double, std::milli>(steady_clock::now() - base).count();
}

namespace detail {
inline std::mutex& mtx() {
  static std::mutex m;
  return m;
}
}  // namespace detail

template <typename... Args>
inline void log(const char* fmt, Args&&... args) {
  if (!enabled().load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(detail::mtx());
  std::fprintf(stderr, "[%9.2f %6ld %-8s] ", now_ms(), tid(), thread_name());
  std::fprintf(stderr, fmt, std::forward<Args>(args)...);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

inline void log(const char* msg) { return log("%s", msg); }

}  // namespace eh::trace
