#pragma once

#include "platform/common/bench/debug_profile.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <string>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace eh::shell::mem {

struct ProcMem {
  long rss_kb = 0;
  long pss_kb = 0;
  long anon_kb = 0;
  long file_kb = 0;
  long shmem_kb = 0;
  // glibc allocator view (0 when unavailable): uordblks = app-held bytes,
  // fordblks = free-but-retained bytes. anon >> uordblks ⇒ fragmentation.
  long mall_uord_kb = 0;
  long mall_ford_kb = 0;
};

inline ProcMem read_proc_mem() {
  ProcMem m;
  // Rss/Pss from smaps_rollup (fast, single snapshot)
  if (std::FILE* f = std::fopen("/proc/self/smaps_rollup", "rb")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      if (std::strncmp(line, "Rss: ", 5) == 0) m.rss_kb = std::atol(line + 5);
      else if (std::strncmp(line, "Pss: ", 5) == 0) m.pss_kb = std::atol(line + 5);
    }
    std::fclose(f);
  }
  // Anon/File/Shmem live in status, NOT in smaps_rollup
  if (std::FILE* f = std::fopen("/proc/self/status", "rb")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      if (std::strncmp(line, "RssAnon:", 8) == 0) m.anon_kb = std::atol(line + 8);
      else if (std::strncmp(line, "RssFile:", 8) == 0) m.file_kb = std::atol(line + 8);
      else if (std::strncmp(line, "RssShmem:", 9) == 0) m.shmem_kb = std::atol(line + 9);
    }
    std::fclose(f);
  }
#ifdef __GLIBC__
  struct mallinfo2 mi = ::mallinfo2();
  m.mall_uord_kb = (long)(mi.uordblks / 1024);
  m.mall_ford_kb = (long)(mi.fordblks / 1024);
#endif
  return m;
}

inline std::uint64_t current_rss_kb() { return (std::uint64_t)read_proc_mem().rss_kb; }

// tag: e.g. "apply", thumb_bytes/icon_bytes from AppState, n_entries visible total.
// No-op unless EH_MEM=1 (or EH_DEBUG master). Prints one line to stderr.
inline void log_mem_breakdown(const char* tag, std::size_t thumb_bytes,
                              std::size_t icon_bytes, std::size_t n_entries) {
  static const bool enabled = [] {
    return eh::debug_profile::env_bool("EH_MEM");
  }();
  if (!enabled) return;
  ProcMem m = read_proc_mem();
  std::fprintf(stderr,
               "[mem] %s rss=%ldMB pss=%ldMB anon=%ldMB file=%ldMB shm=%ldMB "
               "mall_used=%ldMB mall_free=%ldMB "
               "thumbs=%zuMB icons=%zuMB entries=%zu\n",
               tag ? tag : "-", m.rss_kb / 1024, m.pss_kb / 1024,
               m.anon_kb / 1024, m.file_kb / 1024, m.shmem_kb / 1024,
               m.mall_uord_kb / 1024, m.mall_ford_kb / 1024,
               thumb_bytes / (1024 * 1024), icon_bytes / (1024 * 1024),
               n_entries);
}

inline void log_mem_breakdown() {
  log_mem_breakdown("tick", 0, 0, 0);
}

}
