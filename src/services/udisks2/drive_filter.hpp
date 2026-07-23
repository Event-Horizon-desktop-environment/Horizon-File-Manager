#pragma once

// Shared drive/partition filtering logic for sidebar and computer view.
// Follows KDE Dolphin / udisks2 conventions for hiding system partitions.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

namespace eh::drives {

// ── GPT partition type GUIDs to hide (from udisks2 80-udisks2.rules) ──

inline const std::set<std::string>& hidden_partition_guids() {
  static const std::set<std::string> guids = {
      "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", // EFI System Partition
      "21686148-6449-6e6f-744e-656564454649", // BIOS Boot partition
      "a19d880f-05fc-4d3b-a006-743f0f84911e", // RAID partition
      "e6d6d379-f507-44c2-a23c-238f2a3df928", // LVM partition
      "e3c9e316-0b5c-4db8-817d-f92df00215ae", // Microsoft Reserved partition
      "de94bba4-06d1-4d40-a16a-bfd50179d6ac", // Windows Recovery Environment
  };
  return guids;
}

// ── Filesystem types to hide ──

inline bool is_hidden_fstype(const std::string& fs_type) {
  return fs_type == "swap" || fs_type == "squashfs";
}

// ── Mount points to hide ──

inline bool is_hidden_mount_point(const std::string& mp) {
  return mp == "/boot" || mp == "/boot/efi" || mp == "/recovery";
}

// ── Device name patterns to hide ──

inline bool is_hidden_device(const std::string& device) {
  auto name = device.substr(device.find_last_of('/') + 1);
  return name.starts_with("loop") || name.starts_with("zram") ||
         name.starts_with("snap") || name.starts_with("dm-");
}

// ── Partition label strings to hide (fallback when GUID unavailable) ──

inline bool is_hidden_partition_label(const std::string& label) {
  return label == "EFI" ||
         label == "boot" ||
         label == "EFI System Partition" ||
         label == "VTOYEFI" ||
         label == "Microsoft reserved partition" ||
         label == "Windows Recovery Environment" ||
         label == "linux-boot" || label == "linux-efi";
}

// ── Generic system partition labels that should show device size instead ──

inline bool is_generic_partition_label(const std::string& label) {
  return label == "root" ||
         label == "data" ||
         label == "home" ||
         label == "swap";
}

// ── GPT partition type GUID reading ──

/// Read partition type GUIDs from GPT on device.
/// Returns map of partition_number → type_guid (lowercase with dashes).
inline std::map<int, std::string> read_gpt_type_guids(const std::string& device) {
  std::map<int, std::string> result;
  FILE* f = fopen(device.c_str(), "rb");
  if (!f) return result;

  // Read GPT header at LBA 1 (512 bytes)
  uint8_t header[512];
  if (fseek(f, 512, SEEK_SET) != 0 || fread(header, 1, 512, f) != 512) {
    fclose(f);
    return result;
  }

  // Validate "EFI PART" signature at offset 0
  if (memcmp(header, "EFI PART", 8) != 0) {
    fclose(f);
    return result;
  }

  // Header fields (all LE):
  // offset 72: partition_entry_start_lba (uint64)
  // offset 80: num_partition_entries (uint32)
  // offset 84: partition_entry_size (uint32)
  uint64_t entry_start;
  uint32_t num_entries, entry_size;
  memcpy(&entry_start, header + 72, 8);
  memcpy(&num_entries, header + 80, 4);
  memcpy(&entry_size, header + 84, 4);

  if (entry_size < 128 || num_entries == 0) {
    fclose(f);
    return result;
  }

  // Mixed-endian byte map for GUID formatting
  static const uint8_t map[] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  static const char hex[] = "0123456789abcdef";

  auto format_guid = [&](const uint8_t* raw, char* out) {
    char hex_str[33];
    for (int i = 0; i < 16; i++) {
      hex_str[i * 2]     = hex[(raw[map[i]] >> 4) & 0xf];
      hex_str[i * 2 + 1] = hex[raw[map[i]] & 0xf];
    }
    // Format as 8-4-4-4-12 — copy groups from hex_str to non-overlapping out positions
    memcpy(out,      hex_str,      8);  // group 1
    out[8] = '-';
    memcpy(out + 9,  hex_str + 8,  4);  // group 2
    out[13] = '-';
    memcpy(out + 14, hex_str + 12, 4);  // group 3
    out[18] = '-';
    memcpy(out + 19, hex_str + 16, 4);  // group 4
    out[23] = '-';
    memcpy(out + 24, hex_str + 20, 12); // group 5
    out[36] = '\0';
  };

  uint8_t entry[128];
  long base = static_cast<long>(entry_start) * 512;

  for (uint32_t i = 0; i < num_entries; i++) {
    if (fseek(f, base + static_cast<long>(i) * entry_size, SEEK_SET) != 0)
      break;
    if (static_cast<size_t>(fread(entry, 1, 128, f)) != 128)
      break;

    // Skip empty entries (type GUID all zeros)
    bool empty = true;
    for (int j = 0; j < 16; j++) {
      if (entry[j] != 0) { empty = false; break; }
    }
    if (empty) continue;

    char guid[37];
    format_guid(entry, guid);
    result[i + 1] = guid; // partition numbers are 1-based
  }

  fclose(f);
  return result;
}

/// Extract partition number from device path.
/// "/dev/sda2" → 2, "/dev/nvme1n1p3" → 3
inline int extract_partition_number(const std::string& device) {
  auto name = device.substr(device.find_last_of('/') + 1);
  // nvme-style: nvme1n1p3
  auto ppos = name.rfind('p');
  if (ppos != std::string::npos && ppos > 0) {
    bool all_digits = true;
    for (size_t i = ppos + 1; i < name.size(); i++) {
      if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
        all_digits = false;
        break;
      }
    }
    if (all_digits && ppos + 1 < name.size())
      return std::stoi(name.substr(ppos + 1));
  }
  // sd-style: sda2
  if (name.size() >= 4 && name[0] == 's' && name[1] == 'd') {
    auto digits = name.substr(3);
    bool all_digits = true;
    for (char c : digits) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        all_digits = false;
        break;
      }
    }
    if (all_digits && !digits.empty())
      return std::stoi(digits);
  }
  return 0;
}

/// Get the disk device from a partition device.
/// "/dev/nvme1n1p3" → "/dev/nvme1n1", "/dev/sda2" → "/dev/sda"
inline std::string disk_device_from_partition(const std::string& device) {
  auto name = device.substr(device.find_last_of('/') + 1);
  auto prefix = device.substr(0, device.find_last_of('/') + 1);
  // nvme: nvme1n1p3 → nvme1n1
  auto ppos = name.rfind('p');
  if (ppos != std::string::npos && ppos > 0) {
    bool all_digits = true;
    for (size_t i = ppos + 1; i < name.size(); i++) {
      if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
        all_digits = false;
        break;
      }
    }
    if (all_digits && ppos + 1 < name.size())
      return prefix + name.substr(0, ppos);
  }
  // sd: sda2 → sda
  if (name.size() >= 4 && name[0] == 's' && name[1] == 'd')
    return prefix + name.substr(0, 3);
  return device;
}

/// Check if a device should be hidden from the file manager sidebar/computer view.
/// Follows KDE Dolphin / udisks2 filtering conventions.
inline bool should_hide_drive(const std::string& device,
                              const std::string& mount_point = {},
                              const std::string& fs_type = {},
                              const std::string& partition_label = {}) {
  // 1. Hidden device name patterns (loop, zram, snap, device-mapper)
  if (is_hidden_device(device))
    return true;

  // 2. Hidden mount points
  if (!mount_point.empty() && is_hidden_mount_point(mount_point))
    return true;

  // 3. Hidden filesystem types
  if (!fs_type.empty() && is_hidden_fstype(fs_type))
    return true;

  // 4. Hidden partition labels (fallback when GUID not available)
  if (!partition_label.empty() && is_hidden_partition_label(partition_label))
    return true;

  // 5. GPT partition type GUID (primary filter, matches udisks2 rules)
  int part_num = extract_partition_number(device);
  if (part_num > 0) {
    auto disk = disk_device_from_partition(device);
    auto guids = read_gpt_type_guids(disk);
    auto it = guids.find(part_num);
    if (it != guids.end()) {
      if (hidden_partition_guids().count(it->second))
        return true;
    }
  }

  return false;
}

/// Get block device size in bytes from sysfs.
inline uint64_t get_device_size_bytes(const std::string& device) {
  auto name = device.substr(device.find_last_of('/') + 1);
  // Try /sys/class/block/ (works for partitions) then /sys/block/ (whole disks only)
  for (const char* prefix : {"/sys/class/block/", "/sys/block/"}) {
    std::string sysfs_path = std::string(prefix) + name + "/size";
    FILE* f = fopen(sysfs_path.c_str(), "r");
    if (f) {
      uint64_t sectors = 0;
      if (fscanf(f, "%lu", &sectors) == 1) {
        fclose(f);
        return sectors * 512;
      }
      fclose(f);
    }
  }
  return 0;
}

/// Format size in human-readable form like "463.2 GB"
inline std::string format_device_size(uint64_t bytes) {
  if (bytes == 0) return "";
  char buf[32];
  const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  int unit = 0;
  double val = static_cast<double>(bytes);
  while (val >= 1024.0 && unit < 5) { val /= 1024.0; ++unit; }
  if (unit == 0)
    std::snprintf(buf, sizeof(buf), "%.0f %s", val, units[unit]);
  else if (val < 10.0)
    std::snprintf(buf, sizeof(buf), "%.1f %s", val, units[unit]);
  else
    std::snprintf(buf, sizeof(buf), "%.0f %s", val, units[unit]);
  return buf;
}

} // namespace eh::drives
