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
#include <zlib.h>

namespace fs = std::filesystem;

namespace eh::file_browser {

#ifdef EH_HAVE_ZLIB

static // One-shot raw-DEFLATE body for a raw byte run (caller adds gzip framing).
bool gzip_member(const char* data, size_t len, int level, std::string& body) {
  body.clear();
  z_stream strm{};
  if (deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return false;
  char out[65536];
  size_t off = 0;
  int r = Z_OK;
  while (off < len) {
    size_t step = std::min(len - off, size_t(1 << 20));
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data + off));
    strm.avail_in = static_cast<uInt>(step);
    off += step;
    r = Z_OK;
    while (strm.avail_in > 0) {
      strm.next_out = reinterpret_cast<Bytef*>(out);
      strm.avail_out = sizeof(out);
      r = deflate(&strm, Z_NO_FLUSH);
      if (r == Z_STREAM_ERROR) {
        deflateEnd(&strm);
        return false;
      }
      body.append(out, sizeof(out) - strm.avail_out);
    }
  }
  do {
    strm.next_out = reinterpret_cast<Bytef*>(out);
    strm.avail_out = sizeof(out);
    r = deflate(&strm, Z_FINISH);
    if (r != Z_OK && r != Z_STREAM_END) {
      deflateEnd(&strm);
      return false;
    }
    body.append(out, sizeof(out) - strm.avail_out);
  } while (r != Z_STREAM_END);
  deflateEnd(&strm);
  return true;
}

bool engine_compress_tar_gz(const EnginePlan& plan, const std::string& archive_path,
                                   int level, int threads,
                                   const std::shared_ptr<OperationProgress>& prog) {
  level = std::min(9, std::max(0, level));
  prog->total_files.store(plan.file_count);
  prog->total_bytes.store(plan.total_bytes);
  prog->progress.store(0.0);
  prog->copied_files.store(0);
  prog->done_bytes.store(0);

  std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;

  // Chunk tasks: files at/above kSplitMin are split into kChunkSize ranges
  // so one huge file feeds every worker. Each chunk becomes its own gzip
  // member (concatenated members decode as one stream); the first chunk of
  // a file carries the tar header, the last carries the zero pad. Small
  // files stay a single task. Writer emits chunks in plan order.
  struct GzTask {
    size_t ei = 0;      // plan entry index
    uint64_t off = 0;   // byte offset into the file data (not incl. header)
    uint64_t len = 0;   // data bytes in this chunk
    bool first = false;
    bool last = false;
  };
  static constexpr uint64_t kSplitMin = 16ull << 20;
  static constexpr uint64_t kChunkSize = 8ull << 20;
  std::vector<GzTask> tasks;
  std::vector<std::vector<size_t>> file_tasks(plan.entries.size());
  for (size_t i = 0; i < plan.entries.size(); ++i) {
    if (plan.entries[i].kind != 'f') continue;
    uint64_t sz = plan.entries[i].size;
    uint64_t n = 1;
    if (sz >= kSplitMin) n = (sz + kChunkSize - 1) / kChunkSize;
    // Even split for balance: chunk k covers [k*q + min(k,r), +q + (k<r)).
    uint64_t q = sz / n;
    uint64_t r = sz % n;
    for (uint64_t c = 0; c < n; ++c) {
      uint64_t coff = c * q + std::min(c, r);
      uint64_t clen = (c + 1 == n) ? (sz - coff) : (q + (c < r ? 1 : 0));
      file_tasks[i].push_back(tasks.size());
      tasks.push_back(GzTask{i, coff, clen, c == 0, c + 1 == n});
    }
  }

  unsigned workers_n =
      static_cast<unsigned>(std::max(1, compress_effective_threads(threads)));
  if (!tasks.empty())
    workers_n = std::min<unsigned>(workers_n, static_cast<unsigned>(tasks.size()));

  std::vector<std::string> blobs(tasks.size());
  std::vector<char> ready(tasks.size(), 0);
  std::vector<char> failed(tasks.size(), 0);
  std::atomic<size_t> next_task{0};
  std::atomic<uint64_t> buffered{0};
  static constexpr uint64_t kMaxBuffered = 256ull << 20;
  std::mutex mtx;
  std::condition_variable cv;
  std::atomic<bool> worker_error{false};
  std::atomic<bool> writer_done{false};

  // Bar fraction from exact input bytes; callable from workers (per chunk)
  // and the writer (per file). Without the per-chunk call the bar freezes
  // at 0% for the whole of a single huge file. Capped below 1.0: input-read
  // completion is not job completion (finish, handoff, disk write and
  // archive finalization still follow); only the success path stores 1.0.
  auto publish_progress = [&]() {
    if (plan.total_bytes == 0) {
      prog->progress.store(0.0);
      return;
    }
    double f = static_cast<double>(prog->done_bytes.load()) / plan.total_bytes;
    if (f < 0) f = 0;
    if (f > 0.99) f = 0.99;
    prog->progress.store(f);
  };

  auto worker = [&]() {
    char inchunk[1 << 20];
    while (!prog->cancel.load() && !worker_error.load() && !writer_done.load()) {
      size_t t = next_task.fetch_add(1);
      if (t >= tasks.size()) break;
      const GzTask& task = tasks[t];
      size_t ei = task.ei;
      const EngineEntry& e = plan.entries[ei];
      // Stream-deflate one range: first chunk carries the tar header, the
      // last carries the zero pad; each chunk becomes its own gzip member.
      // Raw DEFLATE here; gzip framing is added by hand below.
      z_stream strm{};
      if (deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        worker_error.store(true);
        failed[t] = 1;
        std::lock_guard<std::mutex> lk(mtx);
        ready[t] = 1;
        cv.notify_all();
        break;
      }
      uLong crc = crc32(0L, Z_NULL, 0);
      uint64_t raw_len = 0;
      std::string body;
      char zout[65536];
      bool ok = true;
      auto pump = [&](const char* data, size_t len) {
        if (len == 0 || !ok) return;
        crc = crc_run(crc, data, len);
        raw_len += len;
        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
        strm.avail_in = static_cast<uInt>(len);
        do {
          strm.next_out = reinterpret_cast<Bytef*>(zout);
          strm.avail_out = sizeof(zout);
          int r = deflate(&strm, Z_NO_FLUSH);
          if (r == Z_STREAM_ERROR) { ok = false; return; }
          body.append(zout, sizeof(zout) - strm.avail_out);
        } while (strm.avail_out == 0);
      };
      if (task.first) {
        std::string tar;
        append_tar_header(tar, e.rel, e.size, e.mode, e.mtime, e.uid, e.gid, '0', {});
        pump(tar.data(), tar.size());
      }
      FILE* f = nullptr;
      if (ok && task.len > 0) {
        f = std::fopen(e.full.c_str(), "rb");
        ok = (f != nullptr);
        if (ok && std::fseek(f, static_cast<long>(task.off), SEEK_SET) != 0) ok = false;
      }
      uint64_t remaining = task.len;
      while (ok && remaining > 0 && !prog->cancel.load() && !writer_done.load()) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(inchunk)));
        size_t got = f ? std::fread(inchunk, 1, want, f) : 0;
        if (got == 0) { ok = false; break; } // short read / vanished file
        remaining -= got;
        prog->done_bytes.fetch_add(static_cast<uint64_t>(got));
        publish_progress();
        pump(inchunk, got);
      }
      if (f) std::fclose(f);
      // Zero pad to 512 belongs to the last chunk's member.
      if (ok && task.last) {
        uint64_t pad = (512 - (e.size % 512)) % 512;
        if (pad > 0) {
          static const char zeros[512] = {};
          pump(zeros, static_cast<size_t>(pad));
        }
      }
      // Finish member.
      if (ok) {
        int r;
        do {
          strm.next_out = reinterpret_cast<Bytef*>(zout);
          strm.avail_out = sizeof(zout);
          r = deflate(&strm, Z_FINISH);
          if (r != Z_OK && r != Z_STREAM_END) { ok = false; break; }
          body.append(zout, sizeof(zout) - strm.avail_out);
        } while (r != Z_STREAM_END);
      }
      deflateEnd(&strm);
      if (!ok || prog->cancel.load() || writer_done.load()) {
        if (!prog->cancel.load()) worker_error.store(true);
        failed[t] = 1;
      } else {
        // Gzip framing: header + body + CRC32 + ISIZE (this member's bytes).
        std::string member;
        member.reserve(body.size() + 18);
        const unsigned char gzhead[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
        member.append(reinterpret_cast<const char*>(gzhead), 10);
        member += body;
        uint32_t isize = static_cast<uint32_t>(raw_len & 0xffffffffu);
        char trail[8];
        std::memcpy(trail, &crc, 4);
        std::memcpy(trail + 4, &isize, 4);
        member.append(trail, 8);
        {
          std::unique_lock<std::mutex> lk(mtx);
          cv.wait(lk, [&] {
            return prog->cancel.load() || writer_done.load() ||
                   buffered.load() + member.size() < kMaxBuffered;
          });
          if (prog->cancel.load() || writer_done.load()) break;
          blobs[t] = std::move(member);
          buffered.fetch_add(blobs[t].size());
          ready[t] = 1;
        }
        cv.notify_all();
      }
      if (failed[t]) {
        std::lock_guard<std::mutex> lk(mtx);
        ready[t] = 1;
        cv.notify_all();
      }
    }
  };

  std::vector<std::thread> workers;
  for (unsigned i = 0; i < workers_n && !tasks.empty(); ++i) workers.emplace_back(worker);

  // Writer: plan order. Dirs/symlinks deflate inline (tiny), files take blobs.
  bool write_ok = true;
  int written_files = 0;
  auto write_member = [&](const std::string& raw) {
    if (raw.empty()) return true;
    std::string member;
    if (!gzip_member(raw.data(), raw.size(), level, member)) return false;
    // Frame it.
    uLong crc = crc_run(crc32(0L, Z_NULL, 0), raw.data(), raw.size());
    const unsigned char gzhead[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    out.write(reinterpret_cast<const char*>(gzhead), 10);
    out.write(member.data(), static_cast<std::streamsize>(member.size()));
    uint32_t isize = static_cast<uint32_t>(raw.size() & 0xffffffffu);
    char trail[8];
    std::memcpy(trail, &crc, 4);
    std::memcpy(trail + 4, &isize, 4);
    out.write(trail, 8);
    return static_cast<bool>(out);
  };
  for (size_t i = 0; i < plan.entries.size() && write_ok; ++i) {
    if (prog->cancel.load()) { write_ok = false; break; }
    const EngineEntry& e = plan.entries[i];
    std::string base = fs::path(e.rel).filename().string();
    if (base.size() > 40) base = base.substr(0, 37) + "...";
    prog->set_current_file(base.empty() ? e.rel : base);
    if (e.kind == 'f') {
      for (size_t ti : file_tasks[i]) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return ready[ti] != 0 || prog->cancel.load(); });
        if (prog->cancel.load() || failed[ti]) {
          write_ok = false;
          break;
        }
        std::string member = std::move(blobs[ti]);
        buffered.fetch_sub(member.size());
        lk.unlock();
        cv.notify_all();
        out.write(member.data(), static_cast<std::streamsize>(member.size()));
        if (!out) { write_ok = false; break; }
      }
      if (!write_ok) break;
      ++written_files;
      prog->copied_files.store(written_files);
      publish_progress();
    } else if (e.kind == 'd') {
      std::string raw;
      std::string nm = e.rel.empty() ? "" : (e.rel.back() == '/' ? e.rel : e.rel + "/");
      append_tar_header(raw, nm, 0, e.mode, e.mtime, e.uid, e.gid, '5', {});
      if (!write_member(raw)) { write_ok = false; break; }
    } else if (e.kind == 'l') {
      std::string raw;
      append_tar_header(raw, e.rel, 0, e.mode, e.mtime, e.uid, e.gid, '2', e.target);
      if (!write_member(raw)) { write_ok = false; break; }
    }
  }
  // End-of-archive: two zero blocks as one member.
  if (write_ok && !prog->cancel.load()) {
    std::string raw(1024, '\0');
    if (!write_member(raw)) write_ok = false;
  }
  out.flush();
  out.close();

  {
    std::lock_guard<std::mutex> lk(mtx);
    writer_done.store(true);
  }
  cv.notify_all();
  for (auto& t : workers) t.join();

  if (prog->cancel.load()) return false;
  if (!write_ok || worker_error.load()) return false;
  prog->progress.store(1.0);
  prog->copied_files.store(plan.file_count);
  prog->done_bytes.store(plan.total_bytes);
  return true;
}
#endif // EH_HAVE_ZLIB

} // namespace eh::file_browser
