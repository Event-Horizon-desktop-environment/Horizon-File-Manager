#include "app/file_browser/embed/embed.hpp"

#include <clocale>
#include <cstdio>
#include <cstring>
#include <malloc.h>

int main(int argc, char** argv) {
  // Lock the malloc mmap threshold low: glibc otherwise raises it dynamically
  // after bursty large allocations (e.g. multithreaded compression), parking
  // multi-MB buffers on heap arenas where fragmentation retains them as RSS
  // idle. Locked at 128 KiB, large buffers are mmap'd and actually freed.
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  setlocale(LC_ALL, "");
  if (argc > 1 && std::strcmp(argv[1], "--select-directory") == 0) {
    std::string path;
    int ret = eh::file_browser::run_select_directory(path);
    if (ret == 0 && !path.empty()) {
      std::printf("%s\n", path.c_str());
      std::fflush(stdout);
    }
    return ret;
  }
  if (argc > 1 && std::strcmp(argv[1], "--select-file") == 0) {
    std::string path;
    int ret = eh::file_browser::run_select_file(path);
    if (ret == 0 && !path.empty()) {
      std::printf("%s\n", path.c_str());
      std::fflush(stdout);
    }
    return ret;
  }
  // If the first non-flag argument is a path, navigate to it
  if (argc > 1 && argv[1][0] != '-') {
    return eh::file_browser::run_standalone(argv[1]);
  }
  return eh::file_browser::run_standalone();
}