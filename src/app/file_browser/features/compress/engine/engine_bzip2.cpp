// tar.bz2 and plain tar writers (no codec). One format per translation unit.
#include "app/file_browser/features/compress/compress_engine.hpp"
#include "app/file_browser/features/compress/engine/engine_internal.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/progress/progress.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef EH_HAVE_BZ2
#include <bzlib.h>
#endif

namespace fs = std::filesystem;

namespace eh::file_browser {

namespace {

// Publish the input-read fraction of the bar (capped below 1.0: input
// completion is not job completion; only the success path stores 1.0).
void publish_read_progress(const EnginePlan& plan,
                           const std::shared_ptr<OperationProgress>& prog) {
  if (plan.total_bytes == 0) {
    prog->progress.store(0.0);
    return;
  }
  double f = static_cast<double>(prog->done_bytes.load()) / plan.total_bytes;
  if (f < 0) f = 0;
  if (f > 0.99) f = 0.99;
  prog->progress.store(f);
}

// Walk the plan writing tar blocks to `emit`; shared by the plain-tar and
// tar.bz2 writers. `emit` returns false on write failure.
template <typename Emit>
bool emit_tar_stream(const EnginePlan& plan,
                     const std::shared_ptr<OperationProgress>& prog,
                     Emit&& emit) {
  char inchunk[1 << 20];
  int produced_files = 0;
  for (size_t i = 0; i < plan.entries.size(); ++i) {
    if (prog->cancel.load()) return false;
    const EngineEntry& e = plan.entries[i];
    std::string base = fs::path(e.rel).filename().string();
    if (base.size() > 40) base = base.substr(0, 37) + "...";
    prog->set_current_file(base.empty() ? e.rel : base);
    if (e.kind == 'd') {
      std::string raw;
      std::string nm = e.rel.empty() ? "" : (e.rel.back() == '/' ? e.rel : e.rel + "/");
      append_tar_header(raw, nm, 0, e.mode, e.mtime, e.uid, e.gid, '5', {});
      if (!emit(raw.data(), raw.size())) return false;
    } else if (e.kind == 'l') {
      std::string raw;
      append_tar_header(raw, e.rel, 0, e.mode, e.mtime, e.uid, e.gid, '2', e.target);
      if (!emit(raw.data(), raw.size())) return false;
    } else {
      std::string head;
      append_tar_header(head, e.rel, e.size, e.mode, e.mtime, e.uid, e.gid, '0', {});
      if (!emit(head.data(), head.size())) return false;
      FILE* f = std::fopen(e.full.c_str(), "rb");
      bool ok = (f != nullptr);
      uint64_t remaining = e.size;
      while (ok && remaining > 0 && !prog->cancel.load()) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(inchunk)));
        size_t got = f ? std::fread(inchunk, 1, want, f) : 0;
        if (got == 0) { ok = false; break; } // short read / vanished file
        remaining -= got;
        prog->done_bytes.fetch_add(static_cast<uint64_t>(got));
        publish_read_progress(plan, prog);
        if (!emit(inchunk, got)) { ok = false; break; }
      }
      if (f) std::fclose(f);
      if (!ok) return false;
      uint64_t pad = (512 - (e.size % 512)) % 512;
      if (pad > 0) {
        static const char zeros[512] = {};
        if (!emit(zeros, static_cast<size_t>(pad))) return false;
      }
      ++produced_files;
      prog->copied_files.store(produced_files);
      publish_read_progress(plan, prog);
    }
  }
  if (prog->cancel.load()) return false;
  static const char eoa[1024] = {}; // end-of-archive blocks
  return emit(eoa, sizeof(eoa));
}

} // namespace

bool engine_compress_tar(const EnginePlan& plan, const std::string& archive_path,
                         int level, int threads,
                         const std::shared_ptr<OperationProgress>& prog) {
  (void)level;
  (void)threads; // uncompressed container: no codec knobs
  prog->total_files.store(plan.file_count);
  prog->total_bytes.store(plan.total_bytes);
  prog->progress.store(0.0);
  prog->copied_files.store(0);
  prog->done_bytes.store(0);

  std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  bool ok = emit_tar_stream(plan, prog, [&](const char* d, size_t n) {
    out.write(d, static_cast<std::streamsize>(n));
    return static_cast<bool>(out);
  });
  out.flush();
  out.close();
  if (prog->cancel.load()) return false;
  if (!ok) return false;
  prog->progress.store(1.0);
  prog->copied_files.store(plan.file_count);
  prog->done_bytes.store(plan.total_bytes);
  return true;
}

#ifdef EH_HAVE_BZ2
bool engine_compress_tar_bz2(const EnginePlan& plan, const std::string& archive_path,
                             int level, int threads,
                             const std::shared_ptr<OperationProgress>& prog) {
  (void)threads; // libbz2 has no threaded encoder
  // bz2's level is the block size in 100k units: 1..9.
  int block = std::min(9, std::max(1, level));
  prog->total_files.store(plan.file_count);
  prog->total_bytes.store(plan.total_bytes);
  prog->progress.store(0.0);
  prog->copied_files.store(0);
  prog->done_bytes.store(0);

  std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;

  bz_stream strm{};
  if (BZ2_bzCompressInit(&strm, block, 0, 30) != BZ_OK) return false;

  char zout[65536];
  auto pump = [&](const char* d, size_t n, int action) -> bool {
    strm.next_in = const_cast<char*>(d);
    strm.avail_in = static_cast<unsigned>(n);
    for (;;) {
      strm.next_out = zout;
      strm.avail_out = sizeof(zout);
      int r = BZ2_bzCompress(&strm, action);
      out.write(zout, sizeof(zout) - strm.avail_out);
      if (!out) return false;
      if (r == BZ_STREAM_END) return true;
      if (r != BZ_RUN_OK && r != BZ_FINISH_OK) return false;
      if (action == BZ_RUN && strm.avail_in == 0) return true;
    }
  };

  bool ok = emit_tar_stream(plan, prog, [&](const char* d, size_t n) {
    return n == 0 || pump(d, n, BZ_RUN);
  });
  if (ok) ok = pump(nullptr, 0, BZ_FINISH);
  BZ2_bzCompressEnd(&strm);
  out.flush();
  out.close();
  if (prog->cancel.load()) return false;
  if (!ok) return false;
  prog->progress.store(1.0);
  prog->copied_files.store(plan.file_count);
  prog->done_bytes.store(plan.total_bytes);
  return true;
}
#endif // EH_HAVE_BZ2

} // namespace eh::file_browser
