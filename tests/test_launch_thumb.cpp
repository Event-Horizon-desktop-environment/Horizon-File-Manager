// test_launch_thumb.cpp — unit tests for launch_path + thumb_defer helpers.
#include "app/file_browser/features/launch/launch_path.hpp"
#include "app/file_browser/features/thumbnails/thumb_defer.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <utime.h>
#include <unistd.h>

namespace fs = std::filesystem;
using eh::file_browser::find_cli_launch_target;
using eh::file_browser::normalize_launch_path;
using eh::file_browser::split_launch_target;
using eh::file_browser::thumb_should_defer;

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++failures;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                          \
  } while (0)

static void test_normalize() {
  CHECK(normalize_launch_path("") == "");
  CHECK(normalize_launch_path("  /tmp/x\n") == "/tmp/x");
  CHECK(normalize_launch_path("/home/m/Downloads") == "/home/m/Downloads");
  CHECK(normalize_launch_path("file:///home/m/Downloads/a%20b.mp4") ==
        "/home/m/Downloads/a b.mp4");
  CHECK(normalize_launch_path("file://localhost/home/m/x") == "/home/m/x");
  CHECK(normalize_launch_path("FILE:///tmp/y") == "/tmp/y");
  CHECK(normalize_launch_path("file:///tmp/bad%2Gname") ==
        "/tmp/bad%2Gname");  // invalid escape kept literally
}

static void test_split() {
  char dir_tmpl[] = "/tmp/horizon-launch-test-XXXXXX";
  char* d = mkdtemp(dir_tmpl);
  CHECK(d != nullptr);
  if (!d) return;
  const std::string dir(d);
  const std::string file = dir + "/video.mp4";
  { std::ofstream f(file); f << "x"; }

  std::string out_dir, out_select;
  CHECK(split_launch_target(dir, out_dir, out_select));
  CHECK(out_select.empty());

  CHECK(split_launch_target(file, out_dir, out_select));
  CHECK(out_select == fs::path(file).lexically_normal().string());
  CHECK(fs::is_directory(out_dir));

  // file:// URI of the file also reveals it.
  CHECK(split_launch_target("file://" + file, out_dir, out_select));
  CHECK(out_select == fs::path(file).lexically_normal().string());

  // Vanished mid-download target still opens the parent.
  CHECK(split_launch_target(dir + "/gone.mp4", out_dir, out_select));
  CHECK(out_select.empty());
  CHECK(fs::is_directory(out_dir));

  CHECK(!split_launch_target("", out_dir, out_select));

  std::error_code rm_ec;
  fs::remove_all(dir, rm_ec);
}

static void test_cli_target() {
  char* a0 = const_cast<char*>("horizon-files");
  char* a1 = const_cast<char*>("/tmp/a");
  char* argv1[] = {a0, a1};
  CHECK(find_cli_launch_target(2, argv1) == "/tmp/a");

  char* s = const_cast<char*>("--select");
  char* f = const_cast<char*>("/tmp/b.mp4");
  char* argv2[] = {a0, s, f};
  CHECK(find_cli_launch_target(3, argv2) == "/tmp/b.mp4");

  char* se = const_cast<char*>("--select=/tmp/c.mp4");
  char* argv3[] = {a0, se};
  CHECK(find_cli_launch_target(2, argv3) == "/tmp/c.mp4");

  // Flags are skipped, first positional wins.
  char* fl = const_cast<char*>("--verbose");
  char* argv4[] = {a0, fl, a1};
  CHECK(find_cli_launch_target(3, argv4) == "/tmp/a");

  char* argv5[] = {a0};
  CHECK(find_cli_launch_target(1, argv5).empty());
}

static void test_defer_suffix() {
  // Suffix matches apply without stating the file (rename races).
  CHECK(thumb_should_defer("/tmp/x.part"));
  CHECK(thumb_should_defer("/tmp/x.crdownload"));
  CHECK(thumb_should_defer("/tmp/x.PART"));
  CHECK(thumb_should_defer("/tmp/x.!ut"));
  CHECK(thumb_should_defer("/tmp/x.tmp"));
  CHECK(!thumb_should_defer(""));
  CHECK(!thumb_should_defer("/tmp/video.mp4"));  // missing: decode fails fast
}

static void test_defer_fresh_large() {
  char file_tmpl[] = "/tmp/horizon-defer-test-XXXXXX";
  int fd = mkstemp(file_tmpl);
  CHECK(fd >= 0);
  if (fd < 0) return;
  const std::string path(file_tmpl);
  // 70 MiB sparse: instant, ~no disk backing until written.
  CHECK(ftruncate(fd, 70LL * 1024 * 1024) == 0);
  ::close(fd);

  CHECK(thumb_should_defer(path));  // large + just modified

  // Small files are never deferred, however fresh.
  char small_tmpl[] = "/tmp/horizon-defer-small-XXXXXX";
  int sfd = mkstemp(small_tmpl);
  CHECK(sfd >= 0);
  if (sfd >= 0) {
    ::close(sfd);
    CHECK(!thumb_should_defer(small_tmpl));
    ::unlink(small_tmpl);
  }

  // Age the large file back an hour: settled, decode freely.
  struct utimbuf old_times{};
  old_times.actime = old_times.modtime = 1000000000L;
  CHECK(utime(path.c_str(), &old_times) == 0);
  CHECK(!thumb_should_defer(path));
  ::unlink(path.c_str());
}

int main() {
  test_normalize();
  test_split();
  test_cli_target();
  test_defer_suffix();
  test_defer_fresh_large();
  if (failures == 0) std::printf("launch_thumb: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
