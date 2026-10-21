#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eh::file_browser {

struct OperationProgress;

// Our own in-process compression pipeline: enumerate once, compress blocks
// on a dedicated pool sized to the user's thread choice, assemble containers
// ourselves with linked codec libs (zlib, libbz2, liblzma), exact progress.
// RAR stays external (proprietary algorithm). Formats land one at a time;
// unsupported ones return false from engine_supports_format() and the caller
// keeps the external-tool path.
struct EngineEntry {
  std::string rel;   // archive-relative name, '/' separators
  std::string full;  // absolute source path
  uint64_t size = 0; // regular file size (stale if mutated mid-run; clamped)
  char kind = 'f';   // 'f' regular file, 'd' directory, 'l' symlink
  std::string target; // symlink target for kind 'l'
  unsigned mode = 0644;
  int64_t mtime = 0;
  int uid = 0;
  int gid = 0;
};

struct EnginePlan {
  std::vector<EngineEntry> entries; // parent dirs first, then contents
  uint64_t total_bytes = 0;         // regular file bytes (progress denominator)
  int file_count = 0;               // regular files (panel count)
};

EnginePlan engine_plan_sources(const std::vector<std::string>& paths,
                               const std::string& parent);
bool engine_supports_format(int format_idx);
// Compress plan -> archive_path. Streams progress into prog
// (progress/copied_files/done_bytes/total_*/current_file). True on success,
// false on failure or cancel (caller checks prog->cancel to tell which;
// partial output is left for the caller to remove or keep).
bool engine_compress(const EnginePlan& plan, const std::string& archive_path,
                     int format_idx, int level, int threads,
                     const std::shared_ptr<OperationProgress>& prog);

// In-process extraction using only linked codec libs (zlib/lzma) + our own
// tar/zip/7z parsers — no libarchive, no external tools. Mirrors the
// compress engine: supported extensions return true from
// engine_supports_extract(); engine_extract() returns true on success,
// false on failure, unsupported coder/encryption, or cancel (caller checks
// prog->cancel; partial output stays for the caller to keep or remove).
// Password-protected archives are never attempted (caller must skip the
// engine when a password is present).
bool engine_supports_extract(const std::string& archive_path);
bool engine_extract(const std::string& archive_path,
                    const std::string& dest_dir,
                    const std::shared_ptr<OperationProgress>& prog);
// True when our parsers can see encryption markers (zip bit 0, 7z encoded
// header / AES coder). Tar-family containers have no encryption signal and
// report false. Unknown layouts report false (caller falls back to the
// libarchive probe).
bool engine_archive_is_encrypted(const std::string& archive_path);

} // namespace eh::file_browser
