// horizon-du-scan — single-shot disk-usage enumerator.
//
// Usage: horizon-du-scan <root>
// Stdout: records "<size>\t<dev>\t<ino>\t<kind>\t<path>\0" (kind F file /
// D dir, NUL-terminated so paths with newlines survive).
// Stderr footer: "DU-STATS errors=N pruned=N files=N dirs=N bytes=N\n".
//
// Mirrors the in-process walker rules in
// src/app/file_browser/features/diskusage/scan.cpp: symlinks never
// followed, /proc|/sys|/dev + pseudo-FS pruned (not errors), EACCES/opendir
// failures counted as errors (<Unknown>), hardlink dedupe left to the
// caller (dev+ino shipped per file). Single-threaded on purpose: the main
// process keeps its parallel walker as fallback; this helper exists so the
// N-thread walk + RawFile buffers can live outside horizon-files RSS.
#include <sys/stat.h>
#include <sys/vfs.h>

#include <dirent.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
bool prune_path(const std::string& full) {
  static constexpr const char* kVirt[] = {"/proc", "/sys", "/dev"};
  for (const char* v : kVirt) {
    size_t n = std::strlen(v);
    if (full.size() >= n && full.compare(0, n, v) == 0 &&
        (full.size() == n || full[n] == '/'))
      return true;
  }
  return false;
}

bool is_pseudo_fs(const std::string& path) {
  struct statfs sfs {};
  if (statfs(path.c_str(), &sfs) != 0) return false;
  switch (static_cast<uint32_t>(sfs.f_type)) {
    case 0x9fa0u:     // proc
    case 0x62656572u: // sysfs
    case 0x27e0ebu:   // cgroup v1
    case 0x63677270u: // cgroup v2
    case 0x64626720u: // debugfs
    case 0x74726163u: // tracefs
    case 0x73636673u: // securityfs
    case 0x62656570u: // configfs
    case 0xf97cff8cu: // selinuxfs
    case 0xcafe4e11u: // bpf
    case 0x6e736673u: // nsfs
    case 0x1cd1u:     // devpts
      return true;
    default:
      return false;
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <root>\n", argv[0]);
    return 2;
  }
  const std::string root = argv[1];

  uint64_t errors = 0, pruned = 0, files = 0, dirs = 0, bytes = 0;
  std::vector<std::string> stack;
  stack.push_back(root);

  std::string out;
  out.reserve(65536);
  auto flush = [&] {
    if (!out.empty()) {
      std::fwrite(out.data(), 1, out.size(), stdout);
      out.clear();
    }
  };

  while (!stack.empty()) {
    std::string dir = std::move(stack.back());
    stack.pop_back();
    DIR* d = opendir(dir.c_str());
    if (!d) {
      ++errors;
      continue;
    }
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
      const char* name = de->d_name;
      if (name[0] == '.' &&
          (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        continue;
      std::string full = dir;
      if (!full.empty() && full.back() != '/') full += '/';
      full += name;
      struct stat st {};
      if (lstat(full.c_str(), &st) != 0) {
        ++errors;
        continue;
      }
      if (S_ISLNK(st.st_mode)) continue;
      if (S_ISDIR(st.st_mode)) {
        if (prune_path(full) || is_pseudo_fs(full)) {
          ++pruned;
          continue;
        }
        ++dirs;
        char rec[64];
        int n = std::snprintf(rec, sizeof(rec), "0\t%llu\t%llu\tD\t",
                              (unsigned long long)st.st_dev,
                              (unsigned long long)st.st_ino);
        out.append(rec, static_cast<size_t>(n));
        out += full;
        out += '\0';
        if (out.size() > 60000) flush();
        stack.push_back(std::move(full));
      } else if (S_ISREG(st.st_mode)) {
        ++files;
        bytes += static_cast<uint64_t>(st.st_size);
        char rec[96];
        int n = std::snprintf(rec, sizeof(rec), "%llu\t%llu\t%llu\tF\t",
                              (unsigned long long)st.st_size,
                              (unsigned long long)st.st_dev,
                              (unsigned long long)st.st_ino);
        out.append(rec, static_cast<size_t>(n));
        out += full;
        out += '\0';
        if (out.size() > 60000) flush();
      }
    }
    closedir(d);
  }
  flush();
  std::fflush(stdout);
  std::fprintf(stderr, "DU-STATS errors=%llu pruned=%llu files=%llu dirs=%llu "
                       "bytes=%llu\n",
               (unsigned long long)errors, (unsigned long long)pruned,
               (unsigned long long)files, (unsigned long long)dirs,
               (unsigned long long)bytes);
  return 0;
}
