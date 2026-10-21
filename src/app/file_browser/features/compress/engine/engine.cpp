// Format dispatch for the per-format engine translation units.
#include "app/file_browser/features/compress/compress_engine.hpp"
#include "app/file_browser/features/compress/engine/engine_internal.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/progress/progress.hpp"

namespace eh::file_browser {

bool engine_supports_format(int format_idx) {
#ifdef EH_HAVE_ZLIB
  if (format_idx == 1 || format_idx == 0) return true; // tar.gz + zip
#endif
#ifdef EH_HAVE_LZMA
  if (format_idx == 3 || format_idx == 4) return true; // tar.xz + 7z
#endif
#ifdef EH_HAVE_BZ2
  if (format_idx == 2) return true; // tar.bz2
#endif
  if (format_idx == 6) return true; // plain tar (no codec)
  (void)format_idx;
  return false;
}
bool engine_compress(const EnginePlan& plan, const std::string& archive_path,
                     int format_idx, int level, int threads,
                     const std::shared_ptr<OperationProgress>& prog) {
  prog->total_files.store(plan.file_count);
  prog->total_bytes.store(plan.total_bytes);
  prog->progress.store(0.0);
  prog->copied_files.store(0);
  prog->done_bytes.store(0);
#ifdef EH_HAVE_ZLIB
  if (format_idx == 1)
    return engine_compress_tar_gz(plan, archive_path, level, threads, prog);
  if (format_idx == 0)
    return engine_compress_zip(plan, archive_path, level, threads, prog);
#endif
#ifdef EH_HAVE_LZMA
  if (format_idx == 3)
    return engine_compress_tar_xz(plan, archive_path, level, threads, prog);
  if (format_idx == 4)
    return engine_compress_7z(plan, archive_path, level, threads, prog);
#endif
#ifdef EH_HAVE_BZ2
  if (format_idx == 2)
    return engine_compress_tar_bz2(plan, archive_path, level, threads, prog);
#endif
  if (format_idx == 6)
    return engine_compress_tar(plan, archive_path, level, threads, prog);
  return false;
}
} // namespace eh::file_browser
