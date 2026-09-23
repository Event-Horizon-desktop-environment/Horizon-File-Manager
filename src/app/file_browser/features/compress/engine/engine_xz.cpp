// Split from compress_engine.cpp: one format per translation unit.
#include "app/file_browser/features/compress/compress_engine.hpp"
#include "app/file_browser/features/compress/engine/engine_internal.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/progress/progress.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <lzma.h>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace eh::file_browser {

#ifdef EH_HAVE_LZMA

namespace {

// Bounded tar-chunk queue: single producer (tar assembly) -> consumer (mt encoder).
struct TarChunkQueue {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::string> q;
  uint64_t bytes = 0;
  static constexpr uint64_t kCap = 32ull << 20;
  bool finished = false;

  void push(std::string s, const std::shared_ptr<OperationProgress>& prog) {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] {
      return prog->cancel.load() || bytes + s.size() < kCap;
    });
    if (prog->cancel.load()) return;
    bytes += s.size();
    q.push_back(std::move(s));
    lk.unlock();
    cv.notify_all();
  }

  // False when the producer finished and the queue drained.
  bool pop(std::string& s, const std::shared_ptr<OperationProgress>& prog) {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return prog->cancel.load() || finished || !q.empty(); });
    if (q.empty()) return false;
    s = std::move(q.front());
    q.pop_front();
    bytes -= s.size();
    lk.unlock();
    cv.notify_all();
    return true;
  }

  void set_finished() {
    std::lock_guard<std::mutex> lk(m);
    finished = true;
    cv.notify_all();
  }

  // Wake anyone parked in push/pop (cancel paths break without draining).
  void poke() {
    std::lock_guard<std::mutex> lk(m);
    cv.notify_all();
  }
};

} // namespace
bool engine_compress_tar_xz(const EnginePlan& plan, const std::string& archive_path,
                                   int level, int threads,
                                   const std::shared_ptr<OperationProgress>& prog) {
  level = std::min(9, std::max(0, level));
  unsigned eff =
      static_cast<unsigned>(std::max(1, compress_effective_threads(threads)));

  std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;

  lzma_stream strm = LZMA_STREAM_INIT;
  lzma_mt mt{};
  mt.flags = 0;
  mt.threads = eff;
  mt.block_size = 0; // liblzma auto-split
  mt.timeout = 300;  // like xz CLI: bounded lzma_code() blocking for cancel
  mt.preset = static_cast<uint32_t>(level);
  mt.filters = nullptr;
  mt.check = LZMA_CHECK_CRC64;
  if (lzma_stream_encoder_mt(&strm, &mt) != LZMA_OK) return false;

  TarChunkQueue queue;
  std::atomic<bool> producer_ok{true};

  auto publish = [&]() {
    if (plan.total_bytes == 0) {
      prog->progress.store(0.0);
      return;
    }
    double f = static_cast<double>(prog->done_bytes.load()) / plan.total_bytes;
    if (f < 0) f = 0;
    if (f > 0.99) f = 0.99; // input-read completion is not job completion
    prog->progress.store(f);
  };

  std::thread producer([&]() {
    char inchunk[1 << 20];
    int produced_files = 0;
    for (size_t i = 0; i < plan.entries.size(); ++i) {
      if (prog->cancel.load()) break;
      const EngineEntry& e = plan.entries[i];
      std::string base = fs::path(e.rel).filename().string();
      if (base.size() > 40) base = base.substr(0, 37) + "...";
      prog->set_current_file(base.empty() ? e.rel : base);
      if (e.kind == 'd') {
        std::string raw;
        std::string nm = e.rel.empty() ? "" : (e.rel.back() == '/' ? e.rel : e.rel + "/");
        append_tar_header(raw, nm, 0, e.mode, e.mtime, e.uid, e.gid, '5', {});
        queue.push(std::move(raw), prog);
      } else if (e.kind == 'l') {
        std::string raw;
        append_tar_header(raw, e.rel, 0, e.mode, e.mtime, e.uid, e.gid, '2', e.target);
        queue.push(std::move(raw), prog);
      } else {
        std::string head;
        append_tar_header(head, e.rel, e.size, e.mode, e.mtime, e.uid, e.gid, '0', {});
        queue.push(std::move(head), prog);
        FILE* f = std::fopen(e.full.c_str(), "rb");
        bool ok = (f != nullptr);
        uint64_t remaining = e.size;
        while (ok && remaining > 0 && !prog->cancel.load()) {
          size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(inchunk)));
          size_t got = f ? std::fread(inchunk, 1, want, f) : 0;
          if (got == 0) { ok = false; break; }
          remaining -= got;
          prog->done_bytes.fetch_add(static_cast<uint64_t>(got));
          publish();
          queue.push(std::string(inchunk, got), prog);
        }
        if (f) std::fclose(f);
        uint64_t pad = (512 - (e.size % 512)) % 512;
        if (ok && pad > 0) queue.push(std::string(static_cast<size_t>(pad), '\0'), prog);
        if (!ok) {
          producer_ok.store(false);
          break;
        }
        ++produced_files;
        prog->copied_files.store(produced_files);
        publish();
      }
    }
    if (!prog->cancel.load() && producer_ok.load())
      queue.push(std::string(1024, '\0'), prog); // end-of-archive blocks
    queue.set_finished();
  });

  bool write_ok = true;
  bool stream_end = false;
  char zout[65536];
  std::string chunk;
  bool have_input = false;
  while (!stream_end) {
    if (prog->cancel.load()) { write_ok = false; break; }
    if (!have_input) {
      if (!queue.pop(chunk, prog)) {
        // Drained: finish the stream.
        strm.next_in = nullptr;
        strm.avail_in = 0;
        for (;;) {
          strm.next_out = reinterpret_cast<uint8_t*>(zout);
          strm.avail_out = sizeof(zout);
          lzma_ret r = lzma_code(&strm, LZMA_FINISH);
          out.write(zout, sizeof(zout) - strm.avail_out);
          if (!out) { write_ok = false; break; }
          if (r == LZMA_STREAM_END) { stream_end = true; break; }
          if (r != LZMA_OK || prog->cancel.load()) { write_ok = false; break; }
        }
        break;
      }
      have_input = true;
      strm.next_in = reinterpret_cast<const uint8_t*>(chunk.data());
      strm.avail_in = chunk.size();
    }
    strm.next_out = reinterpret_cast<uint8_t*>(zout);
    strm.avail_out = sizeof(zout);
    lzma_ret r = lzma_code(&strm, LZMA_RUN);
    out.write(zout, sizeof(zout) - strm.avail_out);
    if (!out) { write_ok = false; break; }
    if (r != LZMA_OK) { write_ok = false; break; }
    if (strm.avail_in == 0) have_input = false;
  }
  queue.poke(); // cancel/failure breaks must wake a producer parked in push
  lzma_end(&strm);
  out.flush();
  out.close();

  producer.join();
  if (prog->cancel.load()) return false;
  if (!write_ok || !producer_ok.load()) return false;
  prog->progress.store(1.0);
  prog->copied_files.store(plan.file_count);
  prog->done_bytes.store(plan.total_bytes);
  return true;
}
#endif // EH_HAVE_LZMA

} // namespace eh::file_browser
