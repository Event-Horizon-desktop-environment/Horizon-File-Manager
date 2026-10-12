#include "app/file_browser/embed/embed.hpp"
#include "app/file_browser/features/filemanager1/filemanager1.hpp"
#include "app/file_browser/features/launch/launch_path.hpp"

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
  // Headless FileManager1 service (D-Bus activation when no window runs):
  // own org.freedesktop.FileManager1 and translate browser ShowItems into
  // window launches. Never opens a window itself.
  for (int i = 1; i < argc; ++i) {
    if (!argv[i]) continue;
    if (std::strcmp(argv[i], "--filemanager1-service") == 0 ||
        std::strcmp(argv[i], "--gapplication-service") == 0) {
      eh::file_browser::filemanager1_run_service();
      return 0;
    }
  }
  // Own FileManager1 while this window lives so late browser ShowItems calls
  // spawn windows even after startup (best-effort; silently no-ops when the
  // name is already owned or no session bus exists).
  eh::file_browser::filemanager1_start_in_background();
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
  // Launch targets from browsers/portals/CLI: a directory, a file to reveal
  // in its parent folder (incl. file:// URIs and Nautilus-style --select),
  // or nothing (plain Home). navigate_to() resolves files to parent+select.
  std::string target =
      eh::file_browser::find_cli_launch_target(argc, argv);
  if (!target.empty()) {
    return eh::file_browser::run_standalone(target);
  }
  return eh::file_browser::run_standalone();
}
