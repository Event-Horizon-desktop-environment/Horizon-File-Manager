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
#include <ctime>
#include <zlib.h>

namespace fs = std::filesystem;

namespace eh::file_browser {

#ifdef EH_HAVE_ZLIB

namespace {

void zip_u16(std::string& s, uint16_t v) {
  s += static_cast<char>(v & 0xff);
  s += static_cast<char>((v >> 8) & 0xff);
}

void zip_u32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xff);
}

void zip_u64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s += static_cast<char>((v >> (8 * i)) & 0xff);
}

void zip_dos_time(int64_t mtime, uint16_t& dos_time, uint16_t& dos_date) {
  std::time_t t = static_cast<std::time_t>(mtime);
  std::tm tmv{};
  localtime_r(&t, &tmv);
  int year = tmv.tm_year + 1900;
  if (year < 1980) { dos_time = 0; dos_date = (1 << 5) | 1; return; } // 1980-01-01
  if (year > 2107) { dos_time = 0xFFFF; dos_date = 0xFFFF; return; }
  dos_time = static_cast<uint16_t>(((tmv.tm_hour) << 11) | ((tmv.tm_min) << 5) |
                                   ((tmv.tm_sec / 2) & 0x1f));
  dos_date = static_cast<uint16_t>(((year - 1980) << 9) | ((tmv.tm_mon + 1) << 5) |
                                   (tmv.tm_mday & 0x1f));
}

bool zip_is_utf8_name(const std::string& name) {
  for (unsigned char c : name)
    if (c >= 0x80) return true;
  return false;
}

struct ZipCentral {
  std::string name;
  uint32_t crc = 0;
  uint64_t comp = 0;
  uint64_t uncomp = 0;
  uint64_t offset = 0;
  int64_t mtime = 0;
  uint16_t method = 8;
  uint16_t dos_time = 0;
  uint16_t dos_date = 0;
  uint16_t flags = 0;
  uint32_t ext_attr = 0;
};

void zip_append_local(std::string& entry, const ZipCentral& z, bool zip64,
                      const std::string& extra) {
  entry += "PK\x03\x04";
  zip_u16(entry, zip64 ? 45 : 20);
  zip_u16(entry, z.flags);
  zip_u16(entry, z.method);
  zip_u16(entry, z.dos_time);
  zip_u16(entry, z.dos_date);
  zip_u32(entry, z.crc);
  if (zip64) {
    zip_u32(entry, 0xFFFFFFFFu);
    zip_u32(entry, 0xFFFFFFFFu);
  } else {
    zip_u32(entry, static_cast<uint32_t>(z.comp));
    zip_u32(entry, static_cast<uint32_t>(z.uncomp));
  }
  zip_u16(entry, static_cast<uint16_t>(z.name.size()));
  zip_u16(entry, static_cast<uint16_t>(extra.size()));
  entry += z.name;
  entry += extra;
}

std::string zip_extra64(uint64_t uncomp, bool need_uncomp, uint64_t comp,
                        bool need_comp, uint64_t offset, bool need_offset) {
  std::string body;
  if (need_uncomp) zip_u64(body, uncomp);
  if (need_comp) zip_u64(body, comp);
  if (need_offset) zip_u64(body, offset);
  std::string extra;
  zip_u16(extra, 0x0001);
  zip_u16(extra, static_cast<uint16_t>(body.size()));
  extra += body;
  return extra;
}

std::string zip_extra_ut(int64_t mtime) {
  // Info-ZIP extended timestamp: flags + unix mtime.
  std::string extra;
  zip_u16(extra, 0x5455);
  zip_u16(extra, 5);
  extra += '\x01';
  uint32_t mt = static_cast<uint32_t>(mtime & 0xffffffff);
  zip_u32(extra, mt);
  return extra;
}

void zip_append_central(std::string& cd, const ZipCentral& z, bool zip64,
                        const std::string& extra) {
  cd += "PK\x01\x02";
  zip_u16(cd, (3 << 8) | 63); // Unix, v6.3
  zip_u16(cd, zip64 ? 45 : 20);
  zip_u16(cd, z.flags);
  zip_u16(cd, z.method);
  zip_u16(cd, z.dos_time);
  zip_u16(cd, z.dos_date);
  zip_u32(cd, z.crc);
  if (zip64) {
    zip_u32(cd, 0xFFFFFFFFu);
    zip_u32(cd, 0xFFFFFFFFu);
  } else {
    zip_u32(cd, static_cast<uint32_t>(z.comp));
    zip_u32(cd, static_cast<uint32_t>(z.uncomp));
  }
  zip_u16(cd, static_cast<uint16_t>(z.name.size()));
  zip_u16(cd, static_cast<uint16_t>(extra.size()));
  zip_u16(cd, 0); // comment
  zip_u16(cd, 0); // disk
  zip_u16(cd, 0); // int attr
  zip_u32(cd, z.ext_attr);
  if (zip64)
    zip_u32(cd, 0xFFFFFFFFu);
  else
    zip_u32(cd, static_cast<uint32_t>(z.offset));
  cd += z.name;
  cd += extra;
}

} // namespace
bool engine_compress_zip(const EnginePlan& plan, const std::string& archive_path,
                                int level, int threads,
                                const std::shared_ptr<OperationProgress>& prog) {
  level = std::min(9, std::max(0, level));

  std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;

  // Chunk tasks mirroring the tar.gz path: files at/above kSplitMin split
  // into ranges so one huge file feeds every worker. Raw DEFLATE chunks join
  // with Z_SYNC_FLUSH boundaries (all but the last chunk) into one valid
  // stream; the writer combines per-chunk CRCs. Small files stay one task.
  struct ZipTask {
    size_t ei = 0;
    uint64_t off = 0;
    uint64_t len = 0;
    bool first = false;
    bool last = false;
  };
  static constexpr uint64_t kSplitMin = 16ull << 20;
  static constexpr uint64_t kChunkSize = 8ull << 20;
  std::vector<ZipTask> tasks;
  std::vector<std::vector<size_t>> file_tasks(plan.entries.size());
  for (size_t i = 0; i < plan.entries.size(); ++i) {
    if (plan.entries[i].kind != 'f') continue;
    uint64_t sz = plan.entries[i].size;
    uint64_t n = 1;
    if (sz >= kSplitMin) n = (sz + kChunkSize - 1) / kChunkSize;
    uint64_t q = sz / n;
    uint64_t r = sz % n;
    for (uint64_t c = 0; c < n; ++c) {
      uint64_t coff = c * q + std::min(c, r);
      uint64_t clen = (c + 1 == n) ? (sz - coff) : (q + (c < r ? 1 : 0));
      file_tasks[i].push_back(tasks.size());
      tasks.push_back(ZipTask{i, coff, clen, c == 0, c + 1 == n});
    }
  }

  unsigned workers_n =
      static_cast<unsigned>(std::max(1, compress_effective_threads(threads)));
  if (!tasks.empty())
    workers_n = std::min<unsigned>(workers_n, static_cast<unsigned>(tasks.size()));

  struct Blob {
    std::string data; // raw DEFLATE stream (empty when stored)
    uint32_t crc = 0;
    bool stored = false;
  };
  std::vector<Blob> blobs(tasks.size());
  std::vector<char> ready(tasks.size(), 0);
  std::vector<char> failed(tasks.size(), 0);
  std::atomic<size_t> next_task{0};
  std::atomic<uint64_t> buffered{0};
  static constexpr uint64_t kMaxBuffered = 256ull << 20;
  std::mutex mtx;
  std::condition_variable cv;
  std::atomic<bool> worker_error{false};
  std::atomic<bool> writer_done{false};

  // Same per-chunk bar publishing as the tar.gz path (see above), capped
  // below 1.0 so the bar never claims completion before finalization.
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
      const ZipTask& task = tasks[t];
      size_t ei = task.ei;
      const EngineEntry& e = plan.entries[ei];
      Blob blob;
      bool ok = true;
      if (task.len == 0) {
        blob.stored = true; // empty: store, skip deflate (single chunk only)
      } else {
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
        FILE* f = std::fopen(e.full.c_str(), "rb");
        ok = (f != nullptr);
        if (ok && std::fseek(f, static_cast<long>(task.off), SEEK_SET) != 0) ok = false;
        char zout[65536];
        std::string body;
        uint64_t remaining = task.len;
        while (ok && remaining > 0 && !prog->cancel.load() && !writer_done.load()) {
          size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(inchunk)));
          size_t got = f ? std::fread(inchunk, 1, want, f) : 0;
          if (got == 0) { ok = false; break; }
          crc = crc_run(crc, inchunk, got);
          remaining -= got;
          prog->done_bytes.fetch_add(static_cast<uint64_t>(got));
          publish_progress();
          strm.next_in = reinterpret_cast<Bytef*>(inchunk);
          strm.avail_in = static_cast<uInt>(got);
          do {
            strm.next_out = reinterpret_cast<Bytef*>(zout);
            strm.avail_out = sizeof(zout);
            int r = deflate(&strm, Z_NO_FLUSH);
            if (r == Z_STREAM_ERROR) { ok = false; break; }
            body.append(zout, sizeof(zout) - strm.avail_out);
          } while (strm.avail_out == 0);
        }
        if (f) std::fclose(f);
        if (ok) {
          // Join chunks into one stream: SYNC_FLUSH boundaries everywhere
          // except the file's last chunk, which ends the stream. CRC covers
          // this chunk's bytes; the writer combines per-chunk CRCs in order.
          int flush = task.last ? Z_FINISH : Z_SYNC_FLUSH;
          int r = Z_OK;
          do {
            strm.next_out = reinterpret_cast<Bytef*>(zout);
            strm.avail_out = sizeof(zout);
            r = deflate(&strm, flush);
            if (r == Z_STREAM_ERROR) { ok = false; break; }
            body.append(zout, sizeof(zout) - strm.avail_out);
            if (flush == Z_FINISH && r != Z_OK && r != Z_STREAM_END) { ok = false; break; }
          } while (flush == Z_FINISH ? (r != Z_STREAM_END) : (strm.avail_out == 0));
        }
        deflateEnd(&strm);
        if (ok) {
          blob.crc = crc;
          blob.data = std::move(body);
        }
      }
      if (!ok || prog->cancel.load() || writer_done.load()) {
        if (!prog->cancel.load()) worker_error.store(true);
        failed[t] = 1;
      } else {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] {
          return prog->cancel.load() || writer_done.load() ||
                 buffered.load() + blob.data.size() < kMaxBuffered;
        });
        if (prog->cancel.load() || writer_done.load()) break;
        buffered.fetch_add(blob.data.size());
        blobs[t] = std::move(blob);
        ready[t] = 1;
      }
      if (failed[t]) {
        std::lock_guard<std::mutex> lk(mtx);
        ready[t] = 1;
        cv.notify_all();
      } else {
        cv.notify_all();
      }
    }
  };

  std::vector<std::thread> workers;
  for (unsigned i = 0; i < workers_n && !tasks.empty(); ++i) workers.emplace_back(worker);

  bool write_ok = true;
  int written_files = 0;
  uint64_t offset = 0;
  std::vector<ZipCentral> central;
  central.reserve(plan.entries.size());
  auto commit = [&](const char* data, size_t len) {
    out.write(data, static_cast<std::streamsize>(len));
    if (out) offset += len;
    return static_cast<bool>(out);
  };
  auto commit_str = [&](const std::string& s) { return commit(s.data(), s.size()); };

  for (size_t i = 0; i < plan.entries.size() && write_ok; ++i) {
    if (prog->cancel.load()) { write_ok = false; break; }
    const EngineEntry& e = plan.entries[i];
    std::string base = fs::path(e.rel).filename().string();
    if (base.size() > 40) base = base.substr(0, 37) + "...";
    prog->set_current_file(base.empty() ? e.rel : base);

    ZipCentral z;
    std::string data; // stored bytes or deflated blob
    if (e.kind == 'd') {
      z.name = e.rel.empty() ? "" : (e.rel.back() == '/' ? e.rel : e.rel + "/");
      z.method = 0;
      uint32_t umode = (e.mode | 0040000u);
      z.ext_attr = (umode << 16) | 0x10u;
    } else if (e.kind == 'l') {
      z.name = e.rel;
      z.method = 0;
      data = e.target;
      z.crc = crc_run(crc32(0L, Z_NULL, 0), data.data(), data.size());
      z.ext_attr = (0120777u << 16);
    } else {
      const std::vector<size_t>& chunks = file_tasks[i];
      if (chunks.size() == 1) {
        size_t ti = chunks[0];
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&] { return ready[ti] != 0 || prog->cancel.load(); });
        if (prog->cancel.load() || failed[ti]) {
          write_ok = false;
          break;
        }
        Blob blob = std::move(blobs[ti]);
        buffered.fetch_sub(blob.data.size());
        lk.unlock();
        cv.notify_all();
        z.name = e.rel;
        z.crc = blob.crc;
        if (blob.stored || e.size == 0) {
          z.method = 0;
          // Stored path only happens for empty files; data stays empty.
        } else {
          z.method = 8;
          data = std::move(blob.data);
        }
      } else {
        // Multi-chunk file: stream chunks in order behind a data descriptor
        // (bit 3), so the writer never holds the whole file. Central
        // directory carries the real sizes/CRC afterwards.
        z.name = e.rel;
        z.method = 8;
        z.flags |= (1u << 3);
        z.crc = 0;
        z.comp = 0;
        z.uncomp = 0;
        std::string entry;
        {
          ZipCentral hdr = z;
          std::string extra = zip_extra_ut(e.mtime);
          zip_append_local(entry, hdr, false, extra);
        }
        z.offset = offset;
        if (!commit_str(entry)) { write_ok = false; break; }
        uint32_t acc_crc = 0;
        bool acc_init = false;
        uint64_t acc_comp = 0;
        for (size_t ti : chunks) {
          std::unique_lock<std::mutex> lk(mtx);
          cv.wait(lk, [&] { return ready[ti] != 0 || prog->cancel.load(); });
          if (prog->cancel.load() || failed[ti]) {
            write_ok = false;
            break;
          }
          Blob blob = std::move(blobs[ti]);
          buffered.fetch_sub(blob.data.size());
          lk.unlock();
          cv.notify_all();
          uint64_t clen = tasks[ti].len;
          if (!acc_init) { acc_crc = blob.crc; acc_init = true; }
          else acc_crc = static_cast<uint32_t>(crc32_combine(acc_crc, blob.crc, static_cast<z_off_t>(clen)));
          acc_comp += blob.data.size();
          if (!commit(blob.data.data(), blob.data.size())) { write_ok = false; break; }
        }
        if (!write_ok) break;
        z.crc = acc_crc;
        z.comp = acc_comp;
        z.uncomp = e.size;
        z.mtime = e.mtime;
        zip_dos_time(e.mtime, z.dos_time, z.dos_date);
        if (zip_is_utf8_name(z.name)) z.flags |= (1u << 11);
        z.ext_attr = (e.mode | 0100000u) << 16;
        data.clear(); // already streamed; keep central values from z
        std::string desc;
        zip_u32(desc, z.crc);
        zip_u32(desc, static_cast<uint32_t>(z.comp));
        zip_u32(desc, static_cast<uint32_t>(z.uncomp));
        if (!commit(desc.data(), desc.size())) { write_ok = false; break; }
        central.push_back(z);
        ++written_files;
        prog->copied_files.store(written_files);
        publish_progress();
        continue;
      }
      ++written_files;
      prog->copied_files.store(written_files);
      publish_progress();
    }
    z.uncomp = (e.kind == 'l') ? data.size() : (e.kind == 'f' ? e.size : 0);
    z.comp = (z.method == 0) ? z.uncomp : data.size();
    z.mtime = e.mtime;
    zip_dos_time(e.mtime, z.dos_time, z.dos_date);
    if (zip_is_utf8_name(z.name)) z.flags |= (1u << 11);
    uint32_t umode = e.mode;
    if (e.kind == 'f') umode |= 0100000u;
    if (e.kind == 'f') z.ext_attr = umode << 16;
    z.offset = offset; // local header starts here
    // ZIP64 note: when any field overflows, write the FULL 0x0001 extra
    // (uncomp+comp locally, +offset centrally) like Info-ZIP does. Minimal
    // "only the overflowed fields" extras trip strict parsers (Py 3.14).
    bool zip64 = (z.uncomp >= 0xFFFFFFFFu) || (z.comp >= 0xFFFFFFFFu) ||
                 (z.offset >= 0xFFFFFFFFu);
    std::string extra = zip_extra_ut(e.mtime);
    if (zip64) extra += zip_extra64(z.uncomp, true, z.comp, true, 0, false);
    std::string entry;
    zip_append_local(entry, z, zip64, extra);
    if (!commit_str(entry) || !commit(data.data(), data.size())) {
      write_ok = false;
      break;
    }
    central.push_back(z);
  }

  // Central directory + end records.
  if (write_ok && !prog->cancel.load()) {
    uint64_t cd_offset = offset;
    for (auto& z : central) {
      std::string extra = zip_extra_ut(z.mtime);
      bool zip64 = (z.uncomp >= 0xFFFFFFFFu) || (z.comp >= 0xFFFFFFFFu) ||
                   (z.offset >= 0xFFFFFFFFu);
      if (zip64) extra += zip_extra64(z.uncomp, true, z.comp, true, z.offset, true);
      std::string rec;
      zip_append_central(rec, z, zip64, extra);
      if (!commit_str(rec)) { write_ok = false; break; }
    }
    if (write_ok) {
      uint64_t cd_size = offset - cd_offset;
      bool need64 = central.size() >= 0xFFFFu || cd_size >= 0xFFFFFFFFu ||
                    cd_offset >= 0xFFFFFFFFu;
      for (auto& z : central) {
        if (z.uncomp >= 0xFFFFFFFFu || z.comp >= 0xFFFFFFFFu ||
            z.offset >= 0xFFFFFFFFu) {
          need64 = true;
          break;
        }
      }
      if (need64) {
        uint64_t eocd64_off = offset;
        std::string rec = "PK\x06\x06";
        zip_u64(rec, 44);
        zip_u16(rec, (3 << 8) | 63);
        zip_u16(rec, 45);
        zip_u32(rec, 0);
        zip_u32(rec, 0);
        zip_u64(rec, central.size());
        zip_u64(rec, central.size());
        zip_u64(rec, cd_size);
        zip_u64(rec, cd_offset);
        if (!commit_str(rec)) write_ok = false;
        if (write_ok) {
          std::string loc = "PK\x06\x07";
          zip_u32(loc, 0);
          zip_u64(loc, eocd64_off);
          zip_u32(loc, 1);
          if (!commit_str(loc)) write_ok = false;
        }
      }
      if (write_ok) {
        std::string eocd = "PK\x05\x06";
        auto put16 = [&](uint64_t v, uint64_t lim) {
          zip_u16(eocd, static_cast<uint16_t>(v >= lim ? lim : v));
        };
        auto put32 = [&](uint64_t v, uint64_t lim) {
          zip_u32(eocd, static_cast<uint32_t>(v >= lim ? lim : v));
        };
        put16(0, 0xFFFFu);
        put16(0, 0xFFFFu);
        put16(central.size(), 0xFFFFu);
        put16(central.size(), 0xFFFFu);
        put32(cd_size, 0xFFFFFFFFu);
        put32(cd_offset, 0xFFFFFFFFu);
        put16(0, 0xFFFFu);
        if (!commit_str(eocd)) write_ok = false;
      }
    }
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
