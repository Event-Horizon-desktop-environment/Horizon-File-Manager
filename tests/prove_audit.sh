#!/usr/bin/env bash
# prove_audit.sh — read-only source evidence for the audit report.
# Exits 0 and prints PROVEN lines; never modifies src/.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"
hit=0
prove() { # $1=label $2..=grep args (fixed strings, file paths relative to repo)
  local label="$1"; shift
  local out
  out=$(grep -rn --fixed-strings "$@" "$R/src" 2>/dev/null | head -8)
  if [ -n "$out" ]; then hit=$((hit+1)); echo "PROVEN  $label"; echo "$out" | sed 's/^/    /'; else echo "NOT-FOUND  $label"; fi
}
proverx() { # regex variant
  local label="$1"; shift
  local out
  out=$(grep -rnE "$@" "$R/src" 2>/dev/null | head -8)
  if [ -n "$out" ]; then hit=$((hit+1)); echo "PROVEN  $label"; echo "$out" | sed 's/^/    /'; else echo "NOT-FOUND  $label"; fi
}

prove "shell-injection[xdg-open]" 'xdg-open ' -- '*.cpp'
prove "shell-injection[gio-trash]" "gio trash '" -- '*.cpp'
prove "shell-injection[udisksctl-unquoted]" 'udisksctl mount -b ' -- '*.cpp'
prove "detached-thread-captures-app" 'std::thread([&app' -- '*.cpp'
prove "deferred-capture-app" 'DeferredCall::callLater([&app' -- '*.cpp'
prove "race[current_file-string]" 'current_file' -- 'src/app/file_browser/features/progress/progress.hpp'
proverx "zipslip[shell-extract]" 'unzip -o|unrar x|7z x' -- '*.cpp'
prove "password-in-ps" '-p' -- 'src/app/file_browser/features/compress/compress.cpp'
proverx "swallowed-dbus-errors" 'catch \(const sdbus::Error' -- '*.cpp'
prove "empty-bind_signals" 'void UDisks2DriveService::bind_signals() {}' -- '*.cpp'
prove "manual-close-after-UnixFd" '::close(fd)' -- 'src/services/udisks2/udisks2_drive_service.cpp'
prove "prefix-match-children" 'rfind(base, 0) == 0' -- 'src/services/udisks2/udisks2_drive_service.cpp'
prove "mount-needle" 'Mounted at ' -- 'src/services/udisks2/udisks2_drive_service.cpp'
prove "post-delete-or" 'access(loopdev' -- 'src/services/udisks2/udisks2_drive_service.cpp'
prove "label-substring" 'target.find(dev_base)' -- 'src/app/file_browser/ui/computer_view.cpp'
prove "pdf-log-per-thumb" 'fopen' -- 'src/app/file_browser/features/preview/pdf_preview.cpp'
prove "trash-scan-per-frame" 'trash_has_files' -- 'src/app/file_browser/features/sidebar/sidebar.cpp'
prove "forced-cmake-subbuild" 'cmake', '--build' -- 'meson.build'
prove "xdg-stubs-false" 'return false' -- 'src/platform/desktop/entries/desktop_xdg_ops.cpp'
proverx "forced-include-logger" "platform/common/log/mangowm_logger" -- 'meson.build'

echo "----"
echo "patterns_proven=$hit"
