#include "services/udisks2/udisks2_drive_service.hpp"

#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace eh::drives {

namespace {

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

std::string UDisks2DriveService::mount(const std::string& object_path, const std::string& options) {
  // Fallback: use udisksctl or pmount
  std::string cmd = "udisksctl mount -b " + object_path + " 2>&1";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return {};
  char buf[512];
  std::string result;
  while (fgets(buf, sizeof(buf), f)) result += buf;
  pclose(f);
  // Parse "Mounted at /media/..." from udisksctl output
  auto pos = result.find("Mounted at ");
  if (pos != std::string::npos) {
    auto start = pos + 11;
    auto end = result.find('\n', start);
    if (end != std::string::npos) return result.substr(start, end - start);
    return result.substr(start);
  }
  return {};
}

bool UDisks2DriveService::unmount(const std::string& object_path) {
  std::string cmd = "udisksctl unmount -b " + object_path + " 2>/dev/null";
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
  bool ok = !mount(object_path, options).empty();
  if (cb) cb(ok);
}

void UDisks2DriveService::unmount_async(const std::string& object_path,
                                         std::function<void(bool)> cb) {
  bool ok = unmount(object_path);
  if (cb) cb(ok);
}

void UDisks2DriveService::add_fstab_async(const std::string&, const std::string&,
                                            const std::string&, const std::string&,
                                            std::function<void(bool)> cb) {
  if (cb) cb(false);
}

void UDisks2DriveService::bind_signals() {}

}
