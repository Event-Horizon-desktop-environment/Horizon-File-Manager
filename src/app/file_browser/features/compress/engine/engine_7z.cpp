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
#if defined(EH_HAVE_ZLIB) || defined(EH_HAVE_LZMA)
#include <zlib.h>
#endif
#include <lzma.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace eh::file_browser {

#ifdef EH_HAVE_LZMA

namespace {

void sz_u64(std::string& s, uint64_t v) {
  // 7z "number": leading 1-bits count the trailing LE bytes.
  uint8_t first = 0;
  uint8_t mask = 0x80;
  int i = 0;
  for (; i < 8; ++i) {
    if (v < (uint64_t(1) << (7 * (i + 1)))) {
      first |= static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
      break;
    }
    first |= mask;
    mask = static_cast<uint8_t>(mask >> 1);
  }
  s += static_cast<char>(first);
  if (i == 8) {
    for (int j = 0; j < 8; ++j) s += static_cast<char>((v >> (8 * j)) & 0xFF);
    return;
  }
  for (int j = 0; j < i; ++j) {
    s += static_cast<char>(v & 0xFF);
    v >>= 8;
  }
}

// UTF-8 -> UTF-16LE with surrogate pairs; invalid sequences become U+FFFD.
void sz_utf16le(std::string& s, const std::string& utf8) {
  size_t i = 0;
  auto emit = [&](uint32_t cp) {
    if (cp < 0x10000) {
      if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
      s += static_cast<char>(cp & 0xFF);
      s += static_cast<char>((cp >> 8) & 0xFF);
    } else {
      cp -= 0x10000;
      uint32_t hi = 0xD800 + ((cp >> 10) & 0x3FF);
      uint32_t lo = 0xDC00 + (cp & 0x3FF);
      s += static_cast<char>(hi & 0xFF);
      s += static_cast<char>((hi >> 8) & 0xFF);
      s += static_cast<char>(lo & 0xFF);
      s += static_cast<char>((lo >> 8) & 0xFF);
    }
  };
  while (i < utf8.size()) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = 0xFFFD;
    size_t extra = 0;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    if (extra > 0) {
      if (i + extra >= utf8.size()) { emit(0xFFFD); break; }
      bool bad = false;
      for (size_t k = 1; k <= extra; ++k) {
        unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
        if ((cc & 0xC0) != 0x80) { bad = true; break; }
        cp = (cp << 6) | (cc & 0x3F);
      }
      // Overlong / range / surrogate checks.
      if (!bad) {
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
          bad = true;
      }
      if (bad) { emit(0xFFFD); i += 1; continue; }
      i += 1 + extra;
      emit(cp);
      continue;
    }
    // Lone continuation byte falls here (extra==0 but c>=0x80).
    if (c >= 0x80) { emit(0xFFFD); i += 1; continue; }
    i += 1;
    emit(cp);
  }
}

uint64_t sz_filetime(int64_t unix_sec) {
  if (unix_sec < 0) unix_sec = 0;
  return (static_cast<uint64_t>(unix_sec) + 11644473600ull) * 10000000ull;
}

struct SzBlockFile {
  size_t entry_idx = 0; // index into plan.entries
  uint64_t size = 0;    // unpacked bytes in this block
  uint32_t crc = 0;     // CRC32 of unpacked bytes
};

struct SzBlock {
  std::vector<SzBlockFile> files;
  uint64_t unpack_size = 0;
  uint64_t pack_size = 0;
  uint32_t pack_crc = 0;
  uint32_t folder_crc = 0;
  std::string tmp_path; // spill file with the LZMA stream
};

} // namespace
bool engine_compress_7z(const EnginePlan& plan, const std::string& archive_path,
                               int level, int threads,
                               const std::shared_ptr<OperationProgress>& prog) {
  level = std::min(9, std::max(0, level));
  unsigned eff =
      static_cast<unsigned>(std::max(1, compress_effective_threads(threads)));

  // Solid blocks: group consecutive data files (never split a file).
  // Dirs/empty files attach to the current block for free.
  static constexpr uint64_t kBlockTarget = 256ull << 20;
  std::vector<SzBlock> blocks;
  blocks.emplace_back();
  for (size_t i = 0; i < plan.entries.size(); ++i) {
    const EngineEntry& e = plan.entries[i];
    uint64_t add = (e.kind == 'f' || e.kind == 'l') ? e.size : 0;
    if (e.kind == 'l') add = e.target.size();
    if (!blocks.back().files.empty() && blocks.back().unpack_size + add > kBlockTarget &&
        add > 0) {
      blocks.emplace_back();
    }
    SzBlockFile bf;
    bf.entry_idx = i;
    bf.size = add;
    blocks.back().files.push_back(bf);
    blocks.back().unpack_size += add;
  }
  // Symlink "size": target bytes travel in the solid stream.
  // (Folded into `add` above via e.target.size().)

  // Stream files per block: only entries carrying bytes (non-empty files,
  // non-empty symlink targets) are unpack substreams. Dirs and empty files
  // live only in FilesInfo (7-Zip rule); blocks without streams get no
  // folder and no packed stream at all.
  auto block_streams = [&](const SzBlock& blk) {
    std::vector<size_t> idx;
    for (size_t j = 0; j < blk.files.size(); ++j) {
      const EngineEntry& e = plan.entries[blk.files[j].entry_idx];
      if (e.kind == 'f' && e.size > 0) idx.push_back(j);
      if (e.kind == 'l' && !e.target.empty()) idx.push_back(j);
    }
    return idx;
  };

  lzma_options_lzma preset_opt{};
  if (lzma_lzma_preset(&preset_opt, static_cast<uint32_t>(level)) != LZMA_OK)
    return false;
  uint8_t lzma_prop =
      static_cast<uint8_t>(((preset_opt.pb * 5 + preset_opt.lp) * 9 + preset_opt.lc) & 0xFF);
  uint32_t lzma_dict = preset_opt.dict_size;

  // Temp spill dir: target dir (same fs), distinctive names.
  std::string tmp_base =
      archive_path + ".horizon-7z-part-" + std::to_string(static_cast<long long>(::getpid())) + "-";
  std::atomic<size_t> next_block{0};
  std::atomic<bool> worker_error{false};
  std::mutex tmp_mtx;
  std::vector<std::string> tmp_files; // for cleanup

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

  auto worker = [&]() {
    char inchunk[1 << 20];
    char zout[65536];
    while (!prog->cancel.load() && !worker_error.load()) {
      size_t b = next_block.fetch_add(1);
      if (b >= blocks.size()) break;
      SzBlock& blk = blocks[b];
      if (block_streams(blk).empty()) continue; // empties: no folder or stream
      lzma_filter filters[2];
      filters[0].id = LZMA_FILTER_LZMA1;
      filters[0].options = &preset_opt;
      filters[1].id = LZMA_VLI_UNKNOWN;
      filters[1].options = nullptr;
      lzma_stream strm = LZMA_STREAM_INIT;
      if (lzma_raw_encoder(&strm, filters) != LZMA_OK) {
        worker_error.store(true);
        break;
      }
      // NOTE: preset_opt is read-only shared across threads here; liblzma
      // copies what it needs at encoder init.
      std::string tmp = tmp_base + std::to_string(b);
      FILE* tf = std::fopen(tmp.c_str(), "wb");
      if (!tf) {
        lzma_end(&strm);
        worker_error.store(true);
        break;
      }
      {
        std::lock_guard<std::mutex> lk(tmp_mtx);
        blk.tmp_path = tmp;
        tmp_files.push_back(tmp);
      }
      bool ok = true;
      uLong folder_crc = crc32(0L, Z_NULL, 0);
      auto feed = [&](const char* data, size_t len, uLong* crc_out) {
        size_t off = 0;
        while (off < len && ok && !prog->cancel.load()) {
          size_t step = std::min(len - off, size_t(1 << 20));
          if (crc_out) *crc_out = crc_run(*crc_out, data + off, step);
          strm.next_in = reinterpret_cast<const uint8_t*>(data + off);
          strm.avail_in = step;
          off += step;
          lzma_ret r;
          do {
            strm.next_out = reinterpret_cast<uint8_t*>(zout);
            strm.avail_out = sizeof(zout);
            r = lzma_code(&strm, LZMA_RUN);
            if (r != LZMA_OK) { ok = false; break; }
            if (std::fwrite(zout, 1, sizeof(zout) - strm.avail_out, tf) !=
                sizeof(zout) - strm.avail_out) {
              ok = false;
              break;
            }
          } while (strm.avail_in > 0);
        }
        return ok;
      };
      for (size_t sj : block_streams(blk)) {
        auto& bf = blk.files[sj];
        if (!ok || prog->cancel.load()) break;
        const EngineEntry& e = plan.entries[bf.entry_idx];
        uLong file_crc = crc32(0L, Z_NULL, 0);
        if (e.kind == 'f') {
          FILE* f = std::fopen(e.full.c_str(), "rb");
          if (!f) { ok = false; break; }
          uint64_t remaining = e.size;
          while (remaining > 0 && !prog->cancel.load()) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(inchunk)));
            size_t got = std::fread(inchunk, 1, want, f);
            if (got == 0) { ok = false; break; }
            remaining -= got;
            prog->done_bytes.fetch_add(static_cast<uint64_t>(got));
            publish();
            file_crc = crc_run(file_crc, inchunk, got);
            folder_crc = crc_run(folder_crc, inchunk, got);
            strm.next_in = reinterpret_cast<const uint8_t*>(inchunk);
            strm.avail_in = got;
            lzma_ret r;
            do {
              strm.next_out = reinterpret_cast<uint8_t*>(zout);
              strm.avail_out = sizeof(zout);
              r = lzma_code(&strm, LZMA_RUN);
              if (r != LZMA_OK) { ok = false; break; }
              if (std::fwrite(zout, 1, sizeof(zout) - strm.avail_out, tf) !=
                  sizeof(zout) - strm.avail_out) {
                ok = false;
                break;
              }
            } while (strm.avail_in > 0);
            if (!ok) break;
          }
          std::fclose(f);
          if (remaining != 0) ok = false; // short read / vanished file
        } else { // symlink with target bytes
          file_crc = crc_run(file_crc, e.target.data(), e.target.size());
          folder_crc = crc_run(folder_crc, e.target.data(), e.target.size());
          if (!feed(e.target.data(), e.target.size(), nullptr)) ok = false;
        }
        bf.crc = file_crc;
        std::string base = fs::path(e.rel).filename().string();
        if (base.size() > 40) base = base.substr(0, 37) + "...";
        prog->set_current_file(base.empty() ? e.rel : base);
      }
      if (ok && !prog->cancel.load()) {
        lzma_ret r;
        do {
          strm.next_out = reinterpret_cast<uint8_t*>(zout);
          strm.avail_out = sizeof(zout);
          r = lzma_code(&strm, LZMA_FINISH);
          if (r != LZMA_OK && r != LZMA_STREAM_END) { ok = false; break; }
          if (std::fwrite(zout, 1, sizeof(zout) - strm.avail_out, tf) !=
              sizeof(zout) - strm.avail_out) {
            ok = false;
            break;
          }
        } while (r != LZMA_STREAM_END);
      }
      lzma_end(&strm);
      std::fclose(tf);
      if (!ok || prog->cancel.load()) {
        worker_error.store(true);
        break;
      }
      struct stat st{};
      if (::stat(tmp.c_str(), &st) != 0) {
        worker_error.store(true);
        break;
      }
      blk.pack_size = static_cast<uint64_t>(st.st_size);
      // CRC of the packed stream.
      FILE* pf = std::fopen(tmp.c_str(), "rb");
      uLong pcrc = crc32(0L, Z_NULL, 0);
      if (pf) {
        char cbuf[65536];
        size_t n;
        while ((n = std::fread(cbuf, 1, sizeof(cbuf), pf)) > 0)
          pcrc = crc_run(pcrc, cbuf, n);
        std::fclose(pf);
      } else {
        worker_error.store(true);
        break;
      }
      blk.pack_crc = pcrc;
      blk.folder_crc = folder_crc;
    }
  };

  unsigned workers_n = std::min<unsigned>(eff, static_cast<unsigned>(blocks.size()));
  if (workers_n == 0) workers_n = 1;
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < workers_n; ++i) workers.emplace_back(worker);
  for (auto& t : workers) t.join();

  auto cleanup_tmps = [&]() {
    for (auto& tp : tmp_files) {
      std::error_code ec;
      fs::remove(tp, ec);
    }
  };
  if (prog->cancel.load() || worker_error.load()) {
    cleanup_tmps();
    return false;
  }

  // ── Assemble: signature + packed blocks + end header ──
  std::ofstream out(archive_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    cleanup_tmps();
    return false;
  }
  // Reserve signature space.
  char sigplace[32] = {};
  out.write(sigplace, 32);
  if (!out) {
    cleanup_tmps();
    return false;
  }
  uint64_t pack_off = 0;
  std::vector<uint64_t> pack_sizes;
  std::vector<uint32_t> pack_crcs;
  std::vector<size_t> packed; // blocks that produced a packed stream
  int written_files = 0;
  bool write_ok = true;
  char copybuf[65536];
  for (size_t b = 0; b < blocks.size() && write_ok; ++b) {
    SzBlock& blk = blocks[b];
    if (blk.tmp_path.empty()) continue; // empties-only: no folder or stream
    FILE* tf = std::fopen(blk.tmp_path.c_str(), "rb");
    if (!tf) { write_ok = false; break; }
    size_t n;
    while ((n = std::fread(copybuf, 1, sizeof(copybuf), tf)) > 0) {
      if (prog->cancel.load()) break;
      out.write(copybuf, static_cast<std::streamsize>(n));
      if (!out) { write_ok = false; break; }
    }
    std::fclose(tf);
    if (prog->cancel.load() || !write_ok) { write_ok = false; break; }
    for (size_t sj : block_streams(blk)) {
      const EngineEntry& e = plan.entries[blk.files[sj].entry_idx];
      std::string base = fs::path(e.rel).filename().string();
      if (base.size() > 40) base = base.substr(0, 37) + "...";
      prog->set_current_file(base.empty() ? e.rel : base);
    }
    pack_off += blk.pack_size;
    pack_sizes.push_back(blk.pack_size);
    pack_crcs.push_back(blk.pack_crc);
    packed.push_back(b);
    // Files counted as the writer passes them (in order).
    for (auto& bf : blk.files) {
      if (plan.entries[bf.entry_idx].kind == 'f' ||
          plan.entries[bf.entry_idx].kind == 'l') {
        ++written_files;
      }
    }
    prog->copied_files.store(written_files);
    publish();
  }
  cleanup_tmps();
  if (!write_ok || prog->cancel.load()) return false;

  // ── End header ──
  // Folders exist only for blocks that produced a packed stream (7-Zip
  // drops empties-only groups entirely); substreams count stream files.
  std::string hdr;
  hdr += '\x01'; // kHeader
  std::vector<std::vector<size_t>> packed_streams; // stream file slots per folder
  std::vector<std::vector<uint64_t>> packed_fsizes;
  for (size_t b : packed) {
    packed_streams.push_back(block_streams(blocks[b]));
    std::vector<uint64_t> fs;
    for (size_t sj : packed_streams.back()) {
      const EngineEntry& e = plan.entries[blocks[b].files[sj].entry_idx];
      fs.push_back(e.kind == 'l' ? e.target.size() : e.size);
    }
    packed_fsizes.push_back(std::move(fs));
  }
  if (!packed.empty()) {
    hdr += '\x04'; // kMainStreamsInfo
    // PackInfo
    hdr += '\x06'; // kPackInfo
    sz_u64(hdr, 0); // PackPos
    sz_u64(hdr, pack_sizes.size());
    hdr += '\x09'; // kSize
    for (auto s : pack_sizes) sz_u64(hdr, s);
    hdr += '\x0A'; // kCRC
    hdr += '\x01'; // allDefined
    for (auto c : pack_crcs) {
      hdr += static_cast<char>(c & 0xFF);
      hdr += static_cast<char>((c >> 8) & 0xFF);
      hdr += static_cast<char>((c >> 16) & 0xFF);
      hdr += static_cast<char>((c >> 24) & 0xFF);
    }
    hdr += '\x00'; // kEnd PackInfo
    // UnpackInfo
    hdr += '\x07'; // kUnpackInfo
    hdr += '\x0B'; // kFolder
    sz_u64(hdr, packed.size());
    hdr += '\x00'; // external = inline
    for (size_t folder = 0; folder < packed.size(); ++folder) {
      sz_u64(hdr, 1); // NumCoders
      hdr += '\x23';  // flags: 3-byte id + props
      hdr += '\x03';
      hdr += '\x01';
      hdr += '\x01'; // LZMA method id
      hdr += '\x05'; // props size
      hdr += static_cast<char>(lzma_prop);
      hdr += static_cast<char>(lzma_dict & 0xFF);
      hdr += static_cast<char>((lzma_dict >> 8) & 0xFF);
      hdr += static_cast<char>((lzma_dict >> 16) & 0xFF);
      hdr += static_cast<char>((lzma_dict >> 24) & 0xFF);
    }
    hdr += '\x0C'; // kCodersUnpackSize
    for (size_t folder = 0; folder < packed.size(); ++folder)
      sz_u64(hdr, blocks[packed[folder]].unpack_size);
    hdr += '\x0A'; // kCRC (folder CRCs, always defined)
    hdr += '\x01';
    for (size_t folder = 0; folder < packed.size(); ++folder) {
      uint32_t c = blocks[packed[folder]].folder_crc;
      hdr += static_cast<char>(c & 0xFF);
      hdr += static_cast<char>((c >> 8) & 0xFF);
      hdr += static_cast<char>((c >> 16) & 0xFF);
      hdr += static_cast<char>((c >> 24) & 0xFF);
    }
    hdr += '\x00'; // kEnd UnpackInfo
    // SubStreamsInfo
    hdr += '\x08'; // kSubStreamsInfo
    // Stream-file counts per folder (section iff any folder holds != 1).
    std::vector<size_t> folder_nfiles;
    for (auto& st : packed_streams) folder_nfiles.push_back(st.size());
    bool any_multi = false;
    for (auto n : folder_nfiles)
      if (n != 1) any_multi = true;
    if (any_multi) {
      hdr += '\x0D'; // kNumUnpackStream
      for (auto n : folder_nfiles) sz_u64(hdr, n);
    }
    bool any_gt1 = false;
    for (auto n : folder_nfiles)
      if (n > 1) any_gt1 = true;
    if (any_gt1) {
      hdr += '\x09'; // kSize
      for (size_t folder = 0; folder < packed.size(); ++folder) {
        for (size_t j = 0; j + 1 < packed_fsizes[folder].size(); ++j)
          sz_u64(hdr, packed_fsizes[folder][j]);
      }
    }
    // File CRCs: single-stream folders reuse their folder CRC (7-Zip dedup
    // rule), so they are omitted here; all other stream files are listed in
    // order. The whole section is omitted when nothing follows it.
    {
      std::string crcsec;
      for (size_t folder = 0; folder < packed.size(); ++folder) {
        if (folder_nfiles[folder] == 1) continue;
        for (size_t sj : packed_streams[folder]) {
          uint32_t c = blocks[packed[folder]].files[sj].crc;
          crcsec += static_cast<char>(c & 0xFF);
          crcsec += static_cast<char>((c >> 8) & 0xFF);
          crcsec += static_cast<char>((c >> 16) & 0xFF);
          crcsec += static_cast<char>((c >> 24) & 0xFF);
        }
      }
      if (!crcsec.empty()) {
        hdr += '\x0A'; // kCRC
        hdr += '\x01'; // allDefined
        hdr += crcsec;
      }
    }
    hdr += '\x00'; // kEnd SubStreamsInfo
    hdr += '\x00'; // kEnd MainStreamsInfo
  }
  // FilesInfo
  hdr += '\x05'; // kFilesInfo
  sz_u64(hdr, plan.entries.size());
  // Empty-stream masks.
  std::vector<int> is_empty;
  for (auto& e : plan.entries) {
    bool empty = true;
    if (e.kind == 'f' && e.size > 0) empty = false;
    if (e.kind == 'l' && !e.target.empty()) empty = false;
    is_empty.push_back(empty ? 1 : 0);
  }
  int num_empty = 0;
  for (auto v : is_empty) num_empty += v;
  if (num_empty > 0) {
    hdr += '\x0E'; // kEmptyStream
    std::string mask;
    unsigned char cur = 0;
    int bits = 0;
    for (auto v : is_empty) {
      cur = static_cast<unsigned char>((cur << 1) | (v & 1));
      if (++bits == 8) {
        mask += static_cast<char>(cur);
        cur = 0;
        bits = 0;
      }
    }
    if (bits > 0) {
      cur = static_cast<unsigned char>(cur << (8 - bits));
      mask += static_cast<char>(cur);
    }
    sz_u64(hdr, mask.size());
    hdr += mask;
    // Which empties are real files (vs dirs)?
    bool any_empty_file = false;
    std::string efmask;
    cur = 0;
    bits = 0;
    for (size_t i = 0; i < plan.entries.size(); ++i) {
      if (!is_empty[i]) continue;
      int is_file = (plan.entries[i].kind != 'd') ? 1 : 0;
      if (is_file) any_empty_file = true;
      cur = static_cast<unsigned char>((cur << 1) | is_file);
      if (++bits == 8) {
        efmask += static_cast<char>(cur);
        cur = 0;
        bits = 0;
      }
    }
    if (bits > 0) {
      cur = static_cast<unsigned char>(cur << (8 - bits));
      efmask += static_cast<char>(cur);
    }
    if (any_empty_file) {
      hdr += '\x0F'; // kEmptyFile
      sz_u64(hdr, efmask.size());
      hdr += efmask;
    }
  }
  // Names (inline UTF-16LE, NUL-terminated each).
  {
    std::string blob;
    for (auto& e : plan.entries) {
      sz_utf16le(blob, e.rel);
      blob += '\x00';
      blob += '\x00';
    }
    hdr += '\x11'; // kName
    sz_u64(hdr, blob.size() + 1);
    hdr += '\x00'; // inline switch
    hdr += blob;
  }
  // MTime + attributes, all defined (7z def-vector: id, size, 0x01, 0x00
  // "inline" switch, values).
  {
    hdr += '\x14'; // kMTime
    sz_u64(hdr, 2 + 8 * plan.entries.size());
    hdr += '\x01';
    hdr += '\x00';
    for (auto& e : plan.entries) {
      uint64_t ft = sz_filetime(e.mtime);
      for (int i = 0; i < 8; ++i)
        hdr += static_cast<char>((ft >> (8 * i)) & 0xFF);
    }
    hdr += '\x15'; // kWinAttrib
    sz_u64(hdr, 2 + 4 * plan.entries.size());
    hdr += '\x01';
    hdr += '\x00';
    for (auto& e : plan.entries) {
      uint32_t umode = e.mode;
      // Windows bits mirror 7-Zip's own convention exactly (ARCHIVE for
      // files, DIRECTORY for dirs): readers key mode restoration off these.
      uint32_t win = 0x2080u;
      if (e.kind == 'd') {
        umode |= 0040000u;
        win = 0x10u;
      } else if (e.kind == 'l') {
        umode = 0120777u;
      } else {
        umode |= 0100000u;
      }
      uint32_t attr = (umode << 16) | win;
      for (int i = 0; i < 4; ++i)
        hdr += static_cast<char>((attr >> (8 * i)) & 0xFF);
    }
  }
  hdr += '\x00'; // kEnd FilesInfo
  hdr += '\x00'; // kEnd Header
  out.write(hdr.data(), static_cast<std::streamsize>(hdr.size()));
  if (!out) return false;
  out.flush();
  // Patch the signature.
  uint64_t header_off = pack_off;
  uint64_t header_size = hdr.size();
  uint32_t header_crc = crc32(0L, Z_NULL, 0);
  header_crc = crc_run(header_crc, hdr.data(), hdr.size());
  char sig[32];
  sig[0] = 0x37;
  sig[1] = 0x7A;
  sig[2] = static_cast<char>(0xBC);
  sig[3] = static_cast<char>(0xAF);
  sig[4] = 0x27;
  sig[5] = 0x1C;
  sig[6] = 0x00;
  sig[7] = 0x04;
  for (int i = 0; i < 8; ++i) sig[12 + i] = static_cast<char>((header_off >> (8 * i)) & 0xFF);
  for (int i = 0; i < 8; ++i) sig[20 + i] = static_cast<char>((header_size >> (8 * i)) & 0xFF);
  for (int i = 0; i < 4; ++i) sig[28 + i] = static_cast<char>((header_crc >> (8 * i)) & 0xFF);
  uint32_t start_crc = crc32(0L, Z_NULL, 0);
  start_crc = crc_run(start_crc, sig + 12, 20);
  for (int i = 0; i < 4; ++i) sig[8 + i] = static_cast<char>((start_crc >> (8 * i)) & 0xFF);
  out.seekp(0);
  out.write(sig, 32);
  out.flush();
  out.close();
  if (!out) return false;

  if (prog->cancel.load()) return false;
  prog->progress.store(1.0);
  prog->copied_files.store(plan.file_count);
  prog->done_bytes.store(plan.total_bytes);
  return true;
}
#endif // EH_HAVE_LZMA

} // namespace eh::file_browser
