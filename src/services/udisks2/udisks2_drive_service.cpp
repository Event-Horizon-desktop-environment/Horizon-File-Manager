#include "services/udisks2/udisks2_drive_service.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace eh::drives {

namespace {

// Forward: defined with the ISO loop helpers below.
std::string shell_quote_arg(const std::string& s);

// Detect mounted drives by parsing /proc/mounts
std::vector<DriveInfo> detect_mounted_drives() {
  std::vector<DriveInfo> drives;
  std::ifstream mounts("/proc/mounts");
  std::string line;
  while (std::getline(mounts, line)) {
    // Format: device mount_point fstype options dump pass
    auto dev_end = line.find(' ');
    if (dev_end == std::string::npos) continue;
    std::string device = line.substr(0, dev_end);

    auto rest = line.substr(dev_end + 1);
    auto mp_end = rest.find(' ');
    if (mp_end == std::string::npos) continue;
    std::string mount_point = rest.substr(0, mp_end);

    // Skip non-real filesystems
    if (device.starts_with("/dev/")) {
      DriveInfo di;
      di.device = device;
      di.mount_point = mount_point;
      di.mounted = true;
      di.label = device.substr(5); // strip "/dev/"
      di.object_path = device;
      drives.push_back(std::move(di));
    }
  }
  return drives;
}

}

UDisks2DriveService& UDisks2DriveService::instance() {
  static UDisks2DriveService inst;
  return inst;
}

UDisks2DriveService::UDisks2DriveService() = default;

UDisks2DriveService::~UDisks2DriveService() = default;

void UDisks2DriveService::start() {
  started_ = true;
}

void UDisks2DriveService::set_change_callback(ChangeCallback cb) {
  on_change_ = std::move(cb);
}

std::vector<DriveInfo> UDisks2DriveService::query_drives() {
  return detect_mounted_drives();
}

static std::string dev_to_udisks_path(const std::string& path) {
  if (path.find("/dev/") == 0) {
    auto name = path.substr(5); // strip "/dev/"
    return "/org/freedesktop/UDisks2/block_devices/" + name;
  }
  return path; // already a UDisks2 object path
}

std::string UDisks2DriveService::mount(const std::string& object_path, const std::string& options) {
  try {
    auto conn = sdbus::createSystemBusConnection();
    auto proxy = sdbus::createProxy(*conn,
                                     sdbus::ServiceName{"org.freedesktop.UDisks2"},
                                     sdbus::ObjectPath{dev_to_udisks_path(object_path)});
    std::map<std::string, sdbus::Variant> mount_opts;
    // Honor the requested mount options (e.g. "ro" for disk images) instead
    // of silently mounting everything read-write.
    if (!options.empty())
      mount_opts["options"] = sdbus::Variant{options};
    std::string mount_path;
    proxy->callMethod("Mount")
        .onInterface("org.freedesktop.UDisks2.Filesystem")
        .withArguments(mount_opts)
        .storeResultsTo(mount_path);
    return mount_path;
  } catch (const sdbus::Error& e) {
    fprintf(stderr, "horizon-files: UDisks2 Mount(%s) failed: %s\n",
            object_path.c_str(), e.what());
  }

  // Fallback: use udisksctl. Real output is
  // "Mounted /dev/sdXN at /run/media/$USER/LABEL." — parse the trailing path.
  std::string cmd = "udisksctl mount -b " + shell_quote_arg(object_path) + " 2>&1";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return {};
  char buf[512];
  std::string result;
  while (fgets(buf, sizeof(buf), f)) result += buf;
  pclose(f);
  auto pos = result.find(" at ");
  if (pos != std::string::npos) {
    auto start = pos + 4;
    auto end = result.find('\n', start);
    std::string mp = (end == std::string::npos) ? result.substr(start)
                                                : result.substr(start, end - start);
    while (!mp.empty() && (mp.back() == '.' || mp.back() == ' ')) mp.pop_back();
    if (!mp.empty() && mp[0] == '/') return mp;
  }
  return {};
}

bool UDisks2DriveService::unmount(const std::string& object_path) {
  try {
    auto conn = sdbus::createSystemBusConnection();
    auto proxy = sdbus::createProxy(*conn,
                                     sdbus::ServiceName{"org.freedesktop.UDisks2"},
                                     sdbus::ObjectPath{dev_to_udisks_path(object_path)});
    std::map<std::string, sdbus::Variant> unmount_opts;
    proxy->callMethod("Unmount")
        .onInterface("org.freedesktop.UDisks2.Filesystem")
        .withArguments(unmount_opts);
    return true;
  } catch (const sdbus::Error& e) {
    fprintf(stderr, "horizon-files: UDisks2 Unmount(%s) failed: %s\n",
            object_path.c_str(), e.what());
  }

  // Fallback
  std::string cmd = "udisksctl unmount -b " + shell_quote_arg(object_path) + " 2>/dev/null";
  return std::system(cmd.c_str()) == 0;
}

bool UDisks2DriveService::has_fstab_entry(const std::string&) { return false; }

bool UDisks2DriveService::add_fstab_entry(const std::string&, const std::string&,
                                            const std::string&, const std::string&) {
  return false;
}

void UDisks2DriveService::mount_async(const std::string& object_path,
                                        std::function<void(bool)> cb,
                                        const std::string& options) {
  std::thread([this, object_path, options, cb = std::move(cb)] {
    std::string result = mount(object_path, options);
    if (cb) cb(!result.empty());
  }).detach();
}

void UDisks2DriveService::unmount_async(const std::string& object_path,
                                           std::function<void(bool)> cb) {
  std::thread([this, object_path, cb = std::move(cb)] {
    bool ok = unmount(object_path);
    if (cb) cb(ok);
  }).detach();
}

void UDisks2DriveService::add_fstab_async(const std::string&, const std::string&,
                                            const std::string&, const std::string&,
                                            std::function<void(bool)> cb) {
  if (cb) cb(false);
}

void UDisks2DriveService::bind_signals() {}

// ── ISO loop helpers ───────────────────────────────────────────────

namespace {

std::string canonical_iso_path(const std::string& p) {
  std::error_code ec;
  auto c = fs::weakly_canonical(p, ec);
  if (!ec && !c.empty()) return c.string();
  return fs::absolute(p, ec).string();
}

std::string read_first_line(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  if (f && std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    return line;
  }
  return {};
}

// Parent loop for a device: /dev/loop0p1 -> loop0, /dev/loop0 -> loop0.
std::string loop_base_name(const std::string& dev) {
  auto name = dev.substr(dev.find_last_of('/') + 1);
  if (name.rfind("loop", 0) != 0) return name;
  // Strip partition suffix: loop0p1 -> loop0, loop12p3 -> loop12.
  auto ppos = name.rfind('p');
  if (ppos != std::string::npos && ppos > 4) {
    bool all_digits = ppos + 1 < name.size();
    for (size_t i = ppos + 1; all_digits && i < name.size(); ++i)
      if (!std::isdigit(static_cast<unsigned char>(name[i]))) all_digits = false;
    if (all_digits) {
      std::string base = name.substr(0, ppos);
      if (base.size() > 4 && base.rfind("loop", 0) == 0) {
        bool base_digits = true;
        for (size_t i = 4; i < base.size(); ++i)
          if (!std::isdigit(static_cast<unsigned char>(base[i]))) { base_digits = false; break; }
        if (base_digits) return base;
      }
    }
  }
  return name;
}

std::string shell_quote_arg(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += '\'';
  return out;
}

std::string run_capture(const std::string& cmd) {
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return {};
  char buf[512];
  std::string out;
  while (fgets(buf, sizeof(buf), f)) out += buf;
  pclose(f);
  return out;
}

} // namespace

std::string UDisks2DriveService::loop_backing_file(const std::string& loopdev) {
  std::string base = loop_base_name(loopdev);
  std::string sysfs = "/sys/block/" + base + "/loop/backing_file";
  std::string backing = read_first_line(sysfs);
  return backing;
}

std::string UDisks2DriveService::find_loop_for_file(const std::string& iso_path) {
  std::string want = canonical_iso_path(iso_path);
  if (want.empty()) return {};
  DIR* d = opendir("/sys/block");
  if (!d) return {};
  std::string found;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name.rfind("loop", 0) != 0) continue;
    // Skip partitions (loop0p1): only probe whole loop devices.
    bool is_part = false;
    auto ppos = name.rfind('p');
    if (ppos != std::string::npos && ppos > 4) {
      bool digits = ppos + 1 < name.size();
      for (size_t i = ppos + 1; digits && i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) digits = false;
      is_part = digits;
    }
    if (is_part) continue;
    std::string backing = read_first_line("/sys/block/" + name + "/loop/backing_file");
    if (backing.empty()) continue;
    if (canonical_iso_path(backing) == want) {
      found = "/dev/" + name;
      break;
    }
  }
  closedir(d);
  return found;
}

static std::string proc_mountpoint_for(const std::string& dev) {
  std::ifstream mounts("/proc/mounts");
  std::string line;
  while (std::getline(mounts, line)) {
    auto sp = line.find(' ');
    if (sp == std::string::npos) continue;
    if (line.compare(0, sp, dev) != 0) continue;
    auto rest = line.substr(sp + 1);
    auto mp_end = rest.find(' ');
    if (mp_end == std::string::npos) continue;
    return rest.substr(0, mp_end);
  }
  return {};
}

static std::vector<std::string> loop_partition_devs(const std::string& loopdev) {
  std::vector<std::string> out;
  std::string base = loop_base_name(loopdev);
  DIR* d = opendir(("/sys/block/" + base).c_str());
  if (!d) return out;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string name = e->d_name;
    if (name == base) continue;
    // Exact child match only: <base>p<digits> (loop0p1). A bare prefix match
    // would also hit unrelated loops (loop1 vs loop10/loop11).
    if (name.size() <= base.size() + 1) continue;
    if (name.compare(0, base.size(), base) != 0) continue;
    if (name[base.size()] != 'p') continue;
    bool all_digits = true;
    for (size_t i = base.size() + 1; i < name.size(); ++i)
      if (!std::isdigit(static_cast<unsigned char>(name[i]))) { all_digits = false; break; }
    if (!all_digits) continue;
    out.push_back("/dev/" + name);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::string UDisks2DriveService::mount_iso(const std::string& iso_path) {
  std::error_code ec;
  if (!fs::exists(iso_path, ec)) return {};

  // Reuse an already-attached loop device (idempotent mount).
  std::string existing = find_loop_for_file(iso_path);
  if (!existing.empty()) {
    if (!proc_mountpoint_for(existing).empty()) return proc_mountpoint_for(existing);
    std::string mp = mount(existing);
    if (!mp.empty()) return mp;
    // Partitioned image: try child partitions (loop0p1...).
    for (const auto& part : loop_partition_devs(existing)) {
      if (!proc_mountpoint_for(part).empty()) return proc_mountpoint_for(part);
      std::string pmp = mount(part);
      if (!pmp.empty()) return pmp;
    }
    if (!existing.empty()) return proc_mountpoint_for(existing);
  }

  // Native UDisks2 LoopSetup with fd passing (read-only, like Dolphin -r).
  // Note: sdbus::UnixFd(int) duplicates the fd, so the original stays ours
  // to close; unique_fd below also closes it on the throw path.
  try {
    struct UniqueFd {
      int fd{-1};
      ~UniqueFd() { if (fd >= 0) ::close(fd); }
    } holder{open(iso_path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (holder.fd >= 0) {
      sdbus::UnixFd sfd{holder.fd};
      std::map<std::string, sdbus::Variant> opts;
      opts["read-only"] = sdbus::Variant{true};
      auto conn = sdbus::createSystemBusConnection();
      auto mgr = sdbus::createProxy(*conn, sdbus::ServiceName{"org.freedesktop.UDisks2"},
                                    sdbus::ObjectPath{"/org/freedesktop/UDisks2/Manager"});
      sdbus::ObjectPath loopPath;
      mgr->callMethod("LoopSetup")
          .onInterface("org.freedesktop.UDisks2.Manager")
          .withArguments(sfd, opts)
          .storeResultsTo(loopPath);
      // sfd holds its own dup; closing the original now is safe.
      ::close(holder.fd);
      holder.fd = -1;
      std::string loopStr = loopPath;
      std::string dev = loopStr.substr(loopStr.find_last_of('/') + 1);
      std::string loopdev = "/dev/" + dev;
      // Wait for the kernel device + udev probe (Filesystem iface).
      for (int i = 0; i < 50; ++i) {
        if (::access(loopdev.c_str(), F_OK) == 0) break;
        usleep(100 * 1000);
      }
      // UDisks may need extra time after the device node appears before the
      // Filesystem interface is probed; retry the mount briefly.
      for (int attempt = 0; attempt < 25; ++attempt) {
        std::string mp = mount(loopdev);
        if (!mp.empty()) return mp;
        usleep(200 * 1000);
      }
      for (const auto& part : loop_partition_devs(loopdev)) {
        for (int i = 0; i < 20 && ::access(part.c_str(), F_OK) != 0; ++i) usleep(100 * 1000);
        for (int attempt = 0; attempt < 10; ++attempt) {
          std::string pmp = mount(part);
          if (!pmp.empty()) return pmp;
          usleep(200 * 1000);
        }
      }
      return proc_mountpoint_for(loopdev);
    }
  } catch (const sdbus::Error& e) {
    fprintf(stderr, "horizon-files: UDisks2 LoopSetup(%s) failed: %s\n",
            iso_path.c_str(), e.what());
  } catch (...) {
    fprintf(stderr, "horizon-files: UDisks2 LoopSetup(%s) failed: unknown error\n",
            iso_path.c_str());
  }

  // Fallback: udisksctl loop-setup + mount (no fd passing).
  std::string setup_out = run_capture("udisksctl loop-setup -r -f " + shell_quote_arg(iso_path) + " 2>&1");
  auto pos = setup_out.find("/dev/loop");
  if (pos == std::string::npos) return {};
  std::string loopdev;
  for (size_t i = pos; i < setup_out.size(); ++i) {
    char c = setup_out[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-') loopdev += c;
    else break;
  }
  if (loopdev.empty()) return {};
  std::string mp = mount(loopdev);
  if (!mp.empty()) return mp;
  for (const auto& part : loop_partition_devs(loopdev)) {
    std::string pmp = mount(part);
    if (!pmp.empty()) return pmp;
  }
  return proc_mountpoint_for(loopdev);
}

bool UDisks2DriveService::unmount_iso(const std::string& iso_or_loopdev) {
  std::string loopdev;
  if (iso_or_loopdev.rfind("/org/freedesktop/UDisks2/", 0) == 0) {
    std::string base = iso_or_loopdev.substr(iso_or_loopdev.find_last_of('/') + 1);
    loopdev = "/dev/" + base;
  } else if (iso_or_loopdev.rfind("/dev/loop", 0) == 0) {
    loopdev = loop_base_name(iso_or_loopdev);
    loopdev = "/dev/" + loopdev;
    // If a partition was given, unmount it first, then operate on the parent.
    if (iso_or_loopdev != loopdev) unmount(iso_or_loopdev);
  } else {
    loopdev = find_loop_for_file(iso_or_loopdev);
  }
  if (loopdev.empty()) return false;
  std::string base = loop_base_name(loopdev);
  loopdev = "/dev/" + base;

  // Unmount the loop + any child partitions first (order matters).
  // Always re-check /proc/mounts afterwards: if anything is still mounted,
  // Loop.Delete would fail, so bail out instead of leaking a half-torn-down loop.
  for (const auto& part : loop_partition_devs(loopdev)) {
    if (!proc_mountpoint_for(part).empty()) unmount(part);
  }
  if (!proc_mountpoint_for(loopdev).empty()) unmount(loopdev);
  for (const auto& part : loop_partition_devs(loopdev)) {
    if (!proc_mountpoint_for(part).empty()) return false;
  }
  if (!proc_mountpoint_for(loopdev).empty()) return false;

  // Loop.Delete via D-Bus, fallback to udisksctl loop-delete.
  try {
    auto conn = sdbus::createSystemBusConnection();
    auto proxy = sdbus::createProxy(*conn, sdbus::ServiceName{"org.freedesktop.UDisks2"},
                                    sdbus::ObjectPath{dev_to_udisks_path(loopdev)});
    std::map<std::string, sdbus::Variant> opts;
    proxy->callMethod("Delete").onInterface("org.freedesktop.UDisks2.Loop").withArguments(opts);
    // Success = the loop device is gone. (Checking only the backing-file map
    // would mask failure: report strictly on the device node.)
    return ::access(loopdev.c_str(), F_OK) != 0;
  } catch (const sdbus::Error&) {
  }
  std::string cmd = "udisksctl loop-delete -b " + shell_quote_arg(loopdev) + " >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

void UDisks2DriveService::mount_iso_async(const std::string& iso_path,
                                          std::function<void(bool, std::string)> cb) {
  std::thread([this, iso_path, cb = std::move(cb)] {
    std::string mp = mount_iso(iso_path);
    if (cb) cb(!mp.empty(), mp);
  }).detach();
}

void UDisks2DriveService::unmount_iso_async(const std::string& iso_or_loopdev,
                                            std::function<void(bool)> cb) {
  std::thread([this, iso_or_loopdev, cb = std::move(cb)] {
    bool ok = unmount_iso(iso_or_loopdev);
    if (cb) cb(ok);
  }).detach();
}

}
