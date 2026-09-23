// test_audit_proofs.cpp — executable proof for the audit report.
// DOES NOT touch src/. Pure predicates replicate the exact logic at the
// cited source line; header-only checks use the real drive_filter.hpp.
// A FAIL here proves the bug exists. Exit code 1 = at least one bug proven.
#include <cctype>

#include <cstdio>
#include <string>
#include <vector>

#include "services/udisks2/drive_filter.hpp"

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool cond, const char* name, const char* detail) {
  if (cond) {
    ++g_pass;
    std::printf("PASS  %s\n", name);
  } else {
    ++g_fail;
    std::printf("FAIL  %s -- %s\n", name, detail);
  }
}

// --- Replicates udisks2_drive_service.cpp loop_partition_devs() matching ---
bool prefix_child_match(const std::string& base, const std::string& name) {
  return name.rfind(base, 0) == 0;  // exact predicate from .cpp:291-ish
}

// --- Replicates udisks2_drive_service.cpp mount() fallback parsing ---
std::string fallback_parse_mounted_at(const std::string& result) {
  auto pos = result.find("Mounted at ");  // exact needle from .cpp:110-ish
  if (pos == std::string::npos) return {};
  auto start = pos + 11;
  auto end = result.find('\n', start);
  if (end != std::string::npos) return result.substr(start, end - start);
  return result.substr(start);
}

// --- Replicates compress.cpp is_archive_extension() ext list ---
bool replica_is_archive_extension(const std::string& path) {
  static const char* kExts[] = {".zip",  ".tar.gz", ".tar.bz2",
                                ".tar.xz", ".7z",     ".rar",
                                ".tar"};
  std::string lower = path;
  for (auto& c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const auto* ext : kExts) {
    std::string e = ext;
    if (lower.size() >= e.size() &&
        lower.compare(lower.size() - e.size(), e.size(), e) == 0)
      return true;
  }
  return false;
}

// --- Replicates computer_view.cpp:148 label substring lookup ---
bool replica_label_substring_hit(const std::string& target,
                                 const std::string& dev_base) {
  return target.find(dev_base) != std::string::npos;  // exact predicate
}
bool correct_label_equality_hit(const std::string& target,
                                const std::string& dev_base) {
  // Resolve "../../sda12" -> "sda12", compare for equality.
  std::string base = target;
  auto slash = base.find_last_of('/');
  if (slash != std::string::npos) base = base.substr(slash + 1);
  return base == dev_base;
}

// --- Replicates clipboard handle_selection() map-miss branch ---
struct ReplicaClipboard {
  void* offer = nullptr;
  std::vector<std::string> mimes;
  void on_selection(void* o, bool in_map,
                    const std::vector<std::string>& mapped) {
    if (!o) {
      offer = nullptr;
      mimes.clear();
    } else {
      offer = o;
      if (in_map) mimes = mapped;  // exact behavior: else keeps stale mimes
    }
  }
};

// --- Replicates unmount_iso() post-Delete check ---
bool replica_delete_ok(bool backing_gone, bool loop_still_exists) {
  return backing_gone || !loop_still_exists;  // exact `||` from .cpp:428-ish
}

}  // namespace

int main() {
  // 1. Real header: loop devices are hidden by the base rule.
  check(eh::drives::is_hidden_device("/dev/loop0") == true,
        "hide-rule[drive_filter.hpp:43] loop hidden",
        "expected loop to be hidden");

  // 2. Real header: dm- volumes hidden (audit minor #29).
  check(eh::drives::is_hidden_device("/dev/dm-0") == true,
        "hide-rule[drive_filter.hpp:45] dm hidden",
        "expected dm- to be hidden");

  // 3. BUG: loop_partition_devs prefix match hits unrelated loops.
  {
    bool hit10 = prefix_child_match("loop1", "loop10");
    bool hit11 = prefix_child_match("loop1", "loop11");
    bool hitp1 = prefix_child_match("loop1", "loop1p1");
    check(hit10 == false && hit11 == false && hitp1 == true,
          "bug[drive_service:loop_partition_devs] loop1 must not match loop10/loop11",
          "replica matched loop10/loop11 as children of loop1");
  }

  // 4. BUG: mount() fallback needle never matches real udisksctl output.
  {
    std::string real =
        "Mounted /dev/sda1 at /run/media/matt/LABEL.\n";
    std::string got = fallback_parse_mounted_at(real);
    check(got == "/run/media/matt/LABEL." || got == "/run/media/matt/LABEL",
          "bug[drive_service:mount-fallback] parses real udisksctl output",
          "needle 'Mounted at ' not found in real output; returns empty");
  }

  // 5. BUG: computer-view label lookup matches sda1 inside sda12.
  {
    bool wrong = replica_label_substring_hit("../../sda12", "sda1");
    bool right = correct_label_equality_hit("../../sda12", "sda1");
    check(wrong == false || right == true,
          "bug[computer_view:148] sda1 must not claim sda12 label",
          "substring find() hits '../../sda12' for dev_base 'sda1'");
  }

  // 6. GAP: .tgz extracts (format_extract_cmd) but is never offered.
  {
    bool offered = replica_is_archive_extension("backup.tgz");
    check(offered == true, "gap[compress] .tgz offered for Extract",
          "ext list lacks .tgz while extractor handles it");
  }

  // 7. BUG: selection advances with map miss -> stale mimes kept.
  {
    ReplicaClipboard c;
    c.offer = (void*)0x1;
    c.mimes = {"x-special/gnome-copied-files"};
    c.on_selection((void*)0x2, false, {});
    check(c.mimes.empty(),
          "bug[clipboard:handle_selection] map miss clears mimes",
          "offer advanced to 0x2 but mimes still advertise old type");
  }

  // 8. BUG: post-Delete `||` masks failure.
  {
    bool reported = replica_delete_ok(/*backing_gone=*/true,
                                      /*loop_still_exists=*/true);
    check(reported == false,
          "bug[drive_service:unmount_iso] Delete reports failure when loop remains",
          "'backing gone || loop gone' returns true while /dev/loopN exists");
  }

  std::printf("----\npass=%d fail=%d\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
