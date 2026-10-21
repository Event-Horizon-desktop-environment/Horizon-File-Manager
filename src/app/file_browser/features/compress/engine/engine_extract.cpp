// Custom in-process extraction: our own tar/zip/7z parsers on top of the
// linked codec libs (zlib, liblzma). No libarchive, no external tools.
// Mirrors the compress engine writers (engine_*.cpp) in reverse.
//
// Supported (when the codec is compiled in):
//   .tar      — plain tar (ustar + pax 'x', GNU L/K), no codec
//   .tar.gz/.tgz — concatenated gzip members -> tar (needs EH_HAVE_ZLIB)
//   .tar.bz2/.tbz2 — concatenated bzip2 blocks -> tar (needs EH_HAVE_BZ2)
//   .tar.xz/.txz — xz stream -> tar (needs EH_HAVE_LZMA)
//   .zip      — stored/deflate, zip64, data descriptors (needs EH_HAVE_ZLIB)
//   .7z       — our writer's subset: plain header, single LZMA1 coder per
//               folder (needs EH_HAVE_LZMA)
//
// Anything else (encryption, unknown coders, rar/...) returns false so
// the caller falls back to libarchive / shell tools.
#include "app/file_browser/features/compress/compress_engine.hpp"
#include "app/file_browser/features/compress/engine/engine_internal.hpp"
#include "app/file_browser/features/progress/progress.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#if defined(EH_HAVE_ZLIB) || defined(EH_HAVE_LZMA)
#include <zlib.h>
#endif
#ifdef EH_HAVE_LZMA
#include <lzma.h>
#endif
#ifdef EH_HAVE_BZ2
#include <bzlib.h>
#endif

namespace fs = std::filesystem;

namespace eh::file_browser {
namespace {

// ── path helpers ──────────────────────────────────────────────────────

std::string lower_copy(const std::string& s) {
  std::string o = s;
  for (auto& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

bool ends_with(const std::string& s, const char* suf) {
  size_t n = s.size(), m = std::strlen(suf);
  return n >= m && s.compare(n - m, m, suf) == 0;
}

std::string sanitize(const std::string& raw) {
  std::string p = raw;
  while (!p.empty() && p[0] == '/') p.erase(p.begin());
  std::string seg;
  std::vector<std::string> clean;
  std::string cur;
  for (size_t i = 0; i <= p.size(); ++i) {
    char c = (i < p.size()) ? p[i] : '/';
    if (c == '/') {
      if (cur == "..") {
        if (!clean.empty()) clean.pop_back();
      } else if (!cur.empty() && cur != ".") {
        clean.push_back(cur);
      }
      cur.clear();
    } else {
      cur += c;
    }
  }
  std::string r;
  for (size_t i = 0; i < clean.size(); ++i) {
    if (i) r += '/';
    r += clean[i];
  }
  return r;
}

// True when `full` is inside `dest` (both absolute, lexically normal).
bool inside_dest(const std::string& dest_abs, const std::string& full) {
  std::string d = fs::path(dest_abs).lexically_normal().string();
  std::string f = fs::path(full).lexically_normal().string();
  if (f.size() < d.size()) return false;
  if (f.compare(0, d.size(), d) != 0) return false;
  return f.size() == d.size() || f[d.size()] == '/';
}

bool ensure_parent(const std::string& full) {
  std::error_code ec;
  fs::path p(full);
  auto parent = p.parent_path();
  if (parent.empty()) return true;
  fs::create_directories(parent, ec);
  return !ec;
}

void set_mtime(const std::string& full, int64_t t) {
  if (t <= 0) return;
  struct utimbuf ub{};
  ub.actime = static_cast<time_t>(t);
  ub.modtime = static_cast<time_t>(t);
  ::utime(full.c_str(), &ub);
}

std::string short_base(const std::string& rel) {
  std::string b = fs::path(rel).filename().string();
  if (b.size() > 40) b = b.substr(0, 37) + "...";
  return b.empty() ? rel : b;
}

// ── tar header helpers ────────────────────────────────────────────────

uint64_t parse_octal(const char* p, size_t w) {
  uint64_t v = 0;
  size_t i = 0;
  while (i < w && (p[i] == ' ' || p[i] == '\0')) ++i;
  for (; i < w; ++i) {
    if (p[i] < '0' || p[i] > '7') break;
    v = (v << 3) + static_cast<uint64_t>(p[i] - '0');
  }
  return v;
}

// Returns false on garbage (caller falls back to libarchive).
bool parse_size(const char* p, size_t w, uint64_t& out) {
  if (w == 0) return false;
  if ((static_cast<unsigned char>(p[0]) & 0x80) != 0) {
    // base-256 binary, big-endian, leading 0x80 marker.
    uint64_t v = 0;
    for (size_t i = 1; i < w; ++i) {
      v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    out = v;
    return true;
  }
  // Octal: must contain at least one octal digit.
  bool any = false;
  for (size_t i = 0; i < w; ++i) {
    if (p[i] >= '0' && p[i] <= '7') { any = true; break; }
    if (p[i] != ' ' && p[i] != '\0') return false;
  }
  if (!any) {
    out = 0;
    return true;
  }
  out = parse_octal(p, w);
  return true;
}

bool is_zero_block(const char* b) {
  for (int i = 0; i < 512; ++i)
    if (b[i] != 0) return false;
  return true;
}

unsigned tar_checksum(const char* b) {
  unsigned s = 0;
  for (int i = 0; i < 512; ++i)
    s += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(b[i]);
  return s;
}

// ── byte sources (streaming decompression -> tar) ─────────────────────

struct ByteSource {
  virtual ~ByteSource() = default;
  // Read exactly n bytes unless EOF/failure. Returns false on short read.
  virtual bool read_exact(char* dst, size_t n) = 0;
  virtual bool cancelled() = 0;
  virtual uint64_t compressed_consumed() = 0;
  virtual uint64_t compressed_total() = 0;
};

struct FileSource : ByteSource {
  FILE* f = nullptr;
  uint64_t total = 0;
  std::shared_ptr<OperationProgress> prog;
  FileSource(FILE* f_, uint64_t t, std::shared_ptr<OperationProgress> p)
      : f(f_), total(t), prog(std::move(p)) {}
  bool read_exact(char* dst, size_t n) override {
    size_t got = 0;
    while (got < n) {
      if (prog && prog->cancel.load()) return false;
      size_t r = std::fread(dst + got, 1, n - got, f);
      if (r == 0) return false;
      got += r;
    }
    return true;
  }
  bool cancelled() override { return prog && prog->cancel.load(); }
  uint64_t compressed_consumed() override {
    long o = std::ftell(f);
    return o < 0 ? 0 : static_cast<uint64_t>(o);
  }
  uint64_t compressed_total() override { return total; }
};

#ifdef EH_HAVE_ZLIB
// Streaming gzip reader: parses each member header by hand, inflates raw
// deflate (-15), verifies CRC/ISIZE, continues with concatenated members.
struct GzipSource : ByteSource {
  FILE* f = nullptr;
  uint64_t total = 0;
  std::shared_ptr<OperationProgress> prog;
  z_stream strm{};
  bool strm_init = false;
  bool member_open = false;
  bool eof = false;
  uLong member_crc = 0;
  uint32_t member_len = 0;
  std::string ibuf;
  size_t ipos = 0;

  GzipSource(FILE* f_, uint64_t t, std::shared_ptr<OperationProgress> p)
      : f(f_), total(t), prog(std::move(p)) {
    ibuf.reserve(1 << 20);
  }
  ~GzipSource() override {
    if (strm_init) inflateEnd(&strm);
  }

  bool fill(size_t want) {
    while (ibuf.size() - ipos < want) {
      if (prog && prog->cancel.load()) return false;
      char tmp[65536];
      size_t r = std::fread(tmp, 1, sizeof(tmp), f);
      if (r == 0) return false;
      ibuf.append(tmp, r);
    }
    return true;
  }
  void consume(size_t n) {
    ipos += n;
    if (ipos >= (1 << 20)) {
      ibuf.erase(0, ipos);
      ipos = 0;
    }
  }
  bool open_member() {
    // Need at least 10 bytes for the fixed header.
    if (!fill(10)) {
      eof = true;
      return false;
    }
    const unsigned char* h =
        reinterpret_cast<const unsigned char*>(ibuf.data() + ipos);
    if (h[0] != 0x1f || h[1] != 0x8b || h[2] != 8) return false;
    unsigned flg = h[3];
    size_t off = 10;
    if (!fill(off + 2)) return false;
    if (flg & 0x04) {  // FEXTRA
      if (!fill(off + 2)) return false;
      uint16_t xlen = static_cast<uint16_t>(h[off] | (h[off + 1] << 8));
      // h may be invalidated by fill(); recompute after fill.
      off += 2 + xlen;
      if (!fill(off)) return false;
      h = reinterpret_cast<const unsigned char*>(ibuf.data() + ipos);
    }
    h = reinterpret_cast<const unsigned char*>(ibuf.data() + ipos);
    if (flg & 0x08) {  // FNAME (NUL-terminated)
      size_t k = off;
      for (;;) {
        if (!fill(k + 1)) return false;
        h = reinterpret_cast<const unsigned char*>(ibuf.data() + ipos);
        if (h[k] == 0) { off = k + 1; break; }
        ++k;
        if (k - off > (1 << 20)) return false;
      }
    }
    if (flg & 0x10) {  // FCOMMENT
      size_t k = off;
      for (;;) {
        if (!fill(k + 1)) return false;
        h = reinterpret_cast<const unsigned char*>(ibuf.data() + ipos);
        if (h[k] == 0) { off = k + 1; break; }
        ++k;
        if (k - off > (1 << 20)) return false;
      }
    }
    if (flg & 0x02) {  // FHCRC
      off += 2;
      if (!fill(off)) return false;
    }
    consume(off);
    if (strm_init) {
      inflateEnd(&strm);
      strm_init = false;
    }
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, -15) != Z_OK) return false;
    strm_init = true;
    member_crc = crc32(0L, Z_NULL, 0);
    member_len = 0;
    member_open = true;
    return true;
  }
  // Inflate exactly n decompressed bytes into dst.
  bool read_exact(char* dst, size_t n) override {
    size_t out = 0;
    char zout[65536];
    while (out < n) {
      if (prog && prog->cancel.load()) return false;
      if (eof) return false;
      if (!member_open) {
        // Skip padding zeros between members (our writer emits none, but
        // be lenient); EOF when no more data.
        for (;;) {
          if (ibuf.size() - ipos > 0) break;
          char tmp[65536];
          size_t r = std::fread(tmp, 1, sizeof(tmp), f);
          if (r == 0) { eof = true; return false; }
          ibuf.append(tmp, r);
        }
        if (!open_member()) return false;
      }
      // Need compressed input available.
      if (ibuf.size() - ipos == 0) {
        char tmp[65536];
        size_t r = std::fread(tmp, 1, sizeof(tmp), f);
        if (r == 0) return false;  // truncated member
        ibuf.append(tmp, r);
      }
      size_t avail = ibuf.size() - ipos;
      strm.next_in =
          reinterpret_cast<Bytef*>(const_cast<char*>(ibuf.data() + ipos));
      strm.avail_in = static_cast<uInt>(std::min<size_t>(avail, 1u << 30));
      size_t want = std::min<size_t>(n - out, sizeof(zout));
      // Inflate directly into dst when it fits, else via zout staging.
      bool direct = want == (n - out) && want <= 65536;
      if (direct) {
        strm.next_out = reinterpret_cast<Bytef*>(dst + out);
        strm.avail_out = static_cast<uInt>(want);
      } else {
        strm.next_out = reinterpret_cast<Bytef*>(zout);
        strm.avail_out = static_cast<uInt>(
            std::min<size_t>(sizeof(zout), n - out));
      }
      uInt before_in = strm.avail_in;
      uInt before_out = strm.avail_out;
      int r = inflate(&strm, Z_NO_FLUSH);
      size_t used_in = before_in - strm.avail_in;
      size_t prod = before_out - strm.avail_out;
      consume(used_in);
      if (r == Z_STREAM_END) {
        if (!direct && prod > 0) std::memcpy(dst + out, zout, prod);
        if (prod > 0) {
          member_crc = crc_run(member_crc, dst + out, prod);
          member_len += static_cast<uint32_t>(prod);
        }
        out += prod;
        // Trailer: CRC32 + ISIZE (8 bytes, may be split across reads).
        char trail[8];
        size_t have = 0;
        while (have < 8) {
          if (ibuf.size() - ipos > 0) {
            size_t k = std::min<size_t>(8 - have, ibuf.size() - ipos);
            std::memcpy(trail + have, ibuf.data() + ipos, k);
            consume(k);
            have += k;
          } else {
            size_t rr = std::fread(trail + have, 1, 8 - have, f);
            if (rr == 0) return false;
            have += rr;
          }
        }
        uint32_t want_crc, want_len;
        std::memcpy(&want_crc, trail, 4);
        std::memcpy(&want_len, trail + 4, 4);
        if (want_crc != static_cast<uint32_t>(member_crc)) return false;
        if (want_len != member_len) return false;
        member_open = false;
        continue;
      }
      if (r != Z_OK) return false;
      if (prod == 0) {
        // No progress: need more input; loop to refill.
        if (strm.avail_in > 0) return false;
        continue;
      }
      if (!direct) std::memcpy(dst + out, zout, prod);
      member_crc = crc_run(member_crc, dst + out, prod);
      member_len += static_cast<uint32_t>(prod);
      out += prod;
    }
    return true;
  }
  bool cancelled() override { return prog && prog->cancel.load(); }
  uint64_t compressed_consumed() override {
    long o = std::ftell(f);
    uint64_t base = o < 0 ? 0 : static_cast<uint64_t>(o);
    size_t buffered = ibuf.size() - ipos;
    return base >= buffered ? base - buffered : 0;
  }
  uint64_t compressed_total() override { return total; }
};
#endif

#ifdef EH_HAVE_LZMA
struct XzSource : ByteSource {
  FILE* f = nullptr;
  uint64_t total = 0;
  std::shared_ptr<OperationProgress> prog;
  lzma_stream strm = LZMA_STREAM_INIT;
  bool init = false;
  bool stream_end = false;
  std::string ibuf;
  size_t ipos = 0;
  XzSource(FILE* f_, uint64_t t, std::shared_ptr<OperationProgress> p)
      : f(f_), total(t), prog(std::move(p)) {
    lzma_ret r = lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
    init = (r == LZMA_OK);
  }
  ~XzSource() override {
    if (init) lzma_end(&strm);
  }
  bool read_exact(char* dst, size_t n) override {
    if (!init) return false;
    size_t out = 0;
    while (out < n) {
      if (prog && prog->cancel.load()) return false;
      if (stream_end) return false;
      if (ibuf.size() - ipos == 0) {
        char tmp[65536];
        size_t r = std::fread(tmp, 1, sizeof(tmp), f);
        if (r > 0) {
          ibuf.append(tmp, r);
        } else {
          // No more input: flush decoder.
          strm.next_in = nullptr;
          strm.avail_in = 0;
        }
      } else {
        strm.next_in =
            reinterpret_cast<const uint8_t*>(ibuf.data() + ipos);
        strm.avail_in = ibuf.size() - ipos;
      }
      if (ibuf.size() - ipos > 0) {
        strm.next_in =
            reinterpret_cast<const uint8_t*>(ibuf.data() + ipos);
        strm.avail_in = ibuf.size() - ipos;
      }
      strm.next_out = reinterpret_cast<uint8_t*>(dst + out);
      strm.avail_out = n - out;
      size_t before_in = strm.avail_in;
      size_t before_out = strm.avail_out;
      lzma_ret r = lzma_code(&strm, LZMA_RUN);
      size_t used = before_in - strm.avail_in;
      size_t prod = before_out - strm.avail_out;
      ipos += used;
      if (ipos >= (1 << 20)) {
        ibuf.erase(0, ipos);
        ipos = 0;
      }
      out += prod;
      if (r == LZMA_STREAM_END) {
        stream_end = (out < n);  // trailing bytes after stream = error
        if (out == n) return true;
        return false;
      }
      if (r != LZMA_OK) return false;
      if (prod == 0 && used == 0) {
        // Stalled: need more input but file is exhausted.
        if (std::feof(f)) return false;
      }
    }
    return true;
  }
  bool cancelled() override { return prog && prog->cancel.load(); }
  uint64_t compressed_consumed() override {
    long o = std::ftell(f);
    uint64_t base = o < 0 ? 0 : static_cast<uint64_t>(o);
    size_t buffered = ibuf.size() - ipos;
    return base >= buffered ? base - buffered : 0;
  }
  uint64_t compressed_total() override { return total; }
};
#endif

#ifdef EH_HAVE_BZ2
// Streaming bzip2 reader: decompresses a member, then re-inits and continues
// with the next (bzip2 files are a concatenation of independent members).
struct Bz2Source : ByteSource {
  FILE* f = nullptr;
  uint64_t total = 0;
  std::shared_ptr<OperationProgress> prog;
  bz_stream strm{};
  bool init = false;
  bool member_open = false;
  bool eof = false;
  std::string ibuf;
  size_t ipos = 0;

  Bz2Source(FILE* f_, uint64_t t, std::shared_ptr<OperationProgress> p)
      : f(f_), total(t), prog(std::move(p)) {
    ibuf.reserve(1 << 20);
    init = (BZ2_bzDecompressInit(&strm, 0, 0) == BZ_OK);
    member_open = init;
  }
  ~Bz2Source() override {
    if (init) BZ2_bzDecompressEnd(&strm);
  }
  bool read_exact(char* dst, size_t n) override {
    if (!init) return false;
    size_t out = 0;
    while (out < n) {
      if (prog && prog->cancel.load()) return false;
      if (eof && ibuf.size() - ipos == 0) return false;
      if (member_open) {
        if (ibuf.size() - ipos == 0) {
          char tmp[65536];
          size_t r = std::fread(tmp, 1, sizeof(tmp), f);
          if (r == 0) return false; // truncated member
          ibuf.append(tmp, r);
        }
        size_t avail = ibuf.size() - ipos;
        strm.next_in = const_cast<char*>(ibuf.data() + ipos);
        strm.avail_in = static_cast<unsigned>(std::min<size_t>(avail, 1u << 30));
        strm.next_out = dst + out;
        strm.avail_out = static_cast<unsigned>(std::min<size_t>(n - out, 1u << 30));
        unsigned before_in = strm.avail_in;
        unsigned before_out = strm.avail_out;
        int r = BZ2_bzDecompress(&strm);
        ipos += before_in - strm.avail_in;
        if (ipos >= (1 << 20)) {
          ibuf.erase(0, ipos);
          ipos = 0;
        }
        out += before_out - strm.avail_out;
        if (r == BZ_STREAM_END) {
          // Member done: drop the decoder and open the next one.
          BZ2_bzDecompressEnd(&strm);
          std::memset(&strm, 0, sizeof(strm));
          init = (BZ2_bzDecompressInit(&strm, 0, 0) == BZ_OK);
          if (!init) return false;
          continue;
        }
        if (r != BZ_OK) return false;
        if (strm.avail_in == 0 && before_out == strm.avail_out) {
          // Stalled with all buffered input consumed: refill.
          char tmp[65536];
          size_t rr = std::fread(tmp, 1, sizeof(tmp), f);
          if (rr == 0) {
            eof = true;
            return false;
          }
          ibuf.append(tmp, rr);
        }
      } else {
        // Between members (or at EOF): look for the next bzip2 header
        // ("BZh" + level digit). Trailing padding zeros are tolerated.
        for (;;) {
          if (ibuf.size() - ipos > 0) break;
          if (eof) return false;
          char tmp[65536];
          size_t r = std::fread(tmp, 1, sizeof(tmp), f);
          if (r == 0) {
            eof = true;
            return false;
          }
          ibuf.append(tmp, r);
        }
        // Scan for "BZh" in the buffered bytes (bounded).
        bool found = false;
        for (size_t k = 0; k + 4 <= ibuf.size() - ipos; ++k) {
          const char* p = ibuf.data() + ipos + k;
          if (p[0] == 'B' && p[1] == 'Z' && p[2] == 'h' &&
              p[3] >= '1' && p[3] <= '9') {
            ipos += k;
            found = true;
            break;
          }
          if (k > (1 << 20)) return false;
          if (ipos + k + 4 >= ibuf.size() && !eof) {
            // Might straddle the buffer edge: keep the tail and refill.
            char tmp[65536];
            size_t r = std::fread(tmp, 1, sizeof(tmp), f);
            if (r == 0) {
              eof = true;
              return false;
            }
            ibuf.append(tmp, r);
          }
        }
        if (!found) return false;
        member_open = true;
      }
    }
    return true;
  }
  bool cancelled() override { return prog && prog->cancel.load(); }
  uint64_t compressed_consumed() override {
    long o = std::ftell(f);
    uint64_t base = o < 0 ? 0 : static_cast<uint64_t>(o);
    size_t buffered = ibuf.size() - ipos;
    return base >= buffered ? base - buffered : 0;
  }
  uint64_t compressed_total() override { return total; }
};
#endif

// ── tar extraction core ───────────────────────────────────────────────

struct TarPax {
  std::string path;
  std::string linkpath;
};

bool parse_pax_records(const std::string& data, TarPax& pax) {
  size_t i = 0;
  while (i < data.size()) {
    size_t nl = data.find('\n', i);
    if (nl == std::string::npos) break;
    std::string line = data.substr(i, nl - i);
    i = nl + 1;
    size_t sp = line.find(' ');
    if (sp == std::string::npos) continue;
    size_t eq = line.find('=', sp + 1);
    if (eq == std::string::npos) continue;
    std::string key = line.substr(sp + 1, eq - sp - 1);
    std::string val = line.substr(eq + 1);
    if (key == "path") pax.path = val;
    if (key == "linkpath") pax.linkpath = val;
  }
  return true;
}

// Write one tar entry's data (already positioned after header). `skip`
// discards instead of writing (used for GNU volume headers etc.).
bool copy_entry_data(ByteSource& src, const std::string& full, uint64_t size,
                     const std::shared_ptr<OperationProgress>& prog,
                     bool skip) {
  if (skip) {
    char tmp[65536];
    uint64_t rem = size;
    while (rem > 0) {
      if (prog->cancel.load()) return false;
      size_t k = static_cast<size_t>(std::min<uint64_t>(rem, sizeof(tmp)));
      if (!src.read_exact(tmp, k)) return false;
      rem -= k;
    }
    return true;
  }
  FILE* o = std::fopen(full.c_str(), "wb");
  if (!o) return false;
  char tmp[65536];
  uint64_t rem = size;
  while (rem > 0) {
    if (prog->cancel.load()) {
      std::fclose(o);
      return false;
    }
    size_t k = static_cast<size_t>(std::min<uint64_t>(rem, sizeof(tmp)));
    if (!src.read_exact(tmp, k)) {
      std::fclose(o);
      return false;
    }
    if (std::fwrite(tmp, 1, k, o) != k) {
      std::fclose(o);
      return false;
    }
    rem -= k;
    prog->done_bytes.fetch_add(static_cast<uint64_t>(k));
  }
  std::fclose(o);
  return true;
}

bool extract_tar_stream(ByteSource& src, const std::string& dest_dir,
                        const std::shared_ptr<OperationProgress>& prog,
                        const std::string& dest_abs) {
  char blk[512];
  TarPax pending_pax;
  bool have_pax = false;
  std::string gnu_longname;
  std::string gnu_longlink;
  int files = 0;

  auto publish = [&]() {
    uint64_t tot = src.compressed_total();
    uint64_t got = src.compressed_consumed();
    double f = 0.0;
    if (tot > 0) f = static_cast<double>(got) / static_cast<double>(tot);
    if (f < 0) f = 0;
    if (f > 0.99) f = 0.99;
    prog->progress.store(f);
    prog->copied_files.store(files);
  };

  for (;;) {
    if (prog->cancel.load()) return false;
    if (!src.read_exact(blk, 512)) {
      // Clean EOF exactly at a block boundary is ambiguous: truncated
      // archives also end mid-stream. Treat short read as failure unless
      // we already saw the two zero blocks (handled below via return).
      return false;
    }
    if (is_zero_block(blk)) {
      // End-of-archive: two consecutive zero blocks. Peek the next one.
      char blk2[512];
      if (!src.read_exact(blk2, 512)) return false;  // truncated
      if (!is_zero_block(blk2)) return false;        // garbage after zeros
      prog->progress.store(1.0);
      prog->copied_files.store(files);
      return true;
    }
    // Verify checksum (tolerate unsigned/signed variants like libarchive).
    unsigned stored = static_cast<unsigned>(parse_octal(blk + 148, 8));
    unsigned calc = tar_checksum(blk);
    if (stored != calc) return false;

    char typeflag = blk[156];
    std::string raw_name(blk, 100);
    raw_name = std::string(raw_name.c_str());
    std::string prefix(blk + 345, 155);
    prefix = std::string(prefix.c_str());
    std::string name = prefix.empty() ? raw_name : (prefix + "/" + raw_name);
    std::string linkname(blk + 157, 100);
    linkname = std::string(linkname.c_str());
    uint64_t size = 0, mtime = 0;
    if (!parse_size(blk + 124, 12, size)) return false;
    if (!parse_size(blk + 136, 12, mtime)) return false;
    unsigned mode = static_cast<unsigned>(parse_octal(blk + 100, 8));

    if (typeflag == 'x' || typeflag == 'g') {
      // Pax extended header: data blocks carry key=value records for the
      // *next* entry ('g' applies globally; treat like 'x' for one entry —
      // our writer only emits per-entry 'x').
      std::string data;
      data.resize(static_cast<size_t>(size));
      if (size > 0 && !src.read_exact(data.data(), static_cast<size_t>(size)))
        return false;
      size_t pad = (512 - (size % 512)) % 512;
      if (pad) {
        char pb[512];
        if (!src.read_exact(pb, pad)) return false;
      }
      TarPax px;
      parse_pax_records(data, px);
      pending_pax = px;
      have_pax = true;
      publish();
      continue;
    }
    if (typeflag == 'L') {
      std::string data;
      data.resize(static_cast<size_t>(size));
      if (size > 0 && !src.read_exact(data.data(), static_cast<size_t>(size)))
        return false;
      size_t pad = (512 - (size % 512)) % 512;
      if (pad) {
        char pb[512];
        if (!src.read_exact(pb, pad)) return false;
      }
      // NUL-trimmed long name for the next entry.
      gnu_longname = std::string(data.c_str(), strnlen(data.c_str(), data.size()));
      publish();
      continue;
    }
    if (typeflag == 'K') {
      std::string data;
      data.resize(static_cast<size_t>(size));
      if (size > 0 && !src.read_exact(data.data(), static_cast<size_t>(size)))
        return false;
      size_t pad = (512 - (size % 512)) % 512;
      if (pad) {
        char pb[512];
        if (!src.read_exact(pb, pad)) return false;
      }
      gnu_longlink = std::string(data.c_str(), strnlen(data.c_str(), data.size()));
      publish();
      continue;
    }

    if (!gnu_longname.empty()) {
      name = gnu_longname;
      gnu_longname.clear();
    }
    if (!gnu_longlink.empty()) {
      linkname = gnu_longlink;
      gnu_longlink.clear();
    }
    if (have_pax) {
      if (!pending_pax.path.empty()) name = pending_pax.path;
      if (!pending_pax.linkpath.empty()) linkname = pending_pax.linkpath;
      have_pax = false;
      pending_pax = TarPax{};
    }

    std::string clean = sanitize(name);
    if (clean.empty()) {
      // e.g. "./" or "/": skip data, keep going.
      uint64_t pad = (512 - (size % 512)) % 512;
      char tmp[65536];
      uint64_t rem = size;
      while (rem > 0) {
        size_t k = static_cast<size_t>(std::min<uint64_t>(rem, sizeof(tmp)));
        if (!src.read_exact(tmp, k)) return false;
        rem -= k;
      }
      if (pad) {
        char pb[512];
        if (!src.read_exact(pb, pad)) return false;
      }
      publish();
      continue;
    }
    std::string full = dest_abs + "/" + clean;
    if (!inside_dest(dest_abs, full)) {
      // ZipSlip: skip entry data, refuse to write outside dest.
      uint64_t pad = (512 - (size % 512)) % 512;
      char tmp[65536];
      uint64_t rem = size;
      while (rem > 0) {
        size_t k = static_cast<size_t>(std::min<uint64_t>(rem, sizeof(tmp)));
        if (!src.read_exact(tmp, k)) return false;
        rem -= k;
      }
      if (pad) {
        char pb[512];
        if (!src.read_exact(pb, pad)) return false;
      }
      publish();
      continue;
    }

    prog->set_current_file(short_base(clean));

    if (typeflag == '5') {
      std::error_code ec;
      fs::create_directories(full, ec);
      if (ec) return false;
      if (mode) ::chmod(full.c_str(), static_cast<mode_t>(mode & 07777));
      set_mtime(full, static_cast<int64_t>(mtime));
      // Dirs carry no data.
      publish();
      continue;
    }
    if (typeflag == '2') {
      if (!ensure_parent(full)) return false;
      std::string clean_link = sanitize(linkname);
      // Keep absolute targets as-is (they describe the link, not a write
      // outside dest); only the link path itself must stay inside dest.
      const std::string& target =
          linkname.empty() ? clean_link : linkname;
      ::unlink(full.c_str());
      if (::symlink(target.c_str(), full.c_str()) != 0) return false;
      ++files;
      publish();
      continue;
    }
    if (typeflag == '1') {
      // Hardlink: linkname is archive-relative.
      std::string hl_clean = sanitize(linkname);
      std::string hl_full = dest_abs + "/" + hl_clean;
      if (!ensure_parent(full)) return false;
      ::unlink(full.c_str());
      if (!inside_dest(dest_abs, hl_full)) {
        // Refuse links pointing outside; skip data.
        uint64_t pad = (512 - (size % 512)) % 512;
        char tmp[65536];
        uint64_t rem = size;
        while (rem > 0) {
          size_t k = static_cast<size_t>(std::min<uint64_t>(rem, sizeof(tmp)));
          if (!src.read_exact(tmp, k)) return false;
          rem -= k;
        }
        if (pad) {
          char pb[512];
          if (!src.read_exact(pb, pad)) return false;
        }
        publish();
        continue;
      }
      // Data for hardlinks is zero-length per spec; consume padding only.
      uint64_t pad = (512 - (size % 512)) % 512;
      if (pad) {
        char pb[512];
        if (!src.read_exact(pb, pad)) return false;
      }
      if (::link(hl_full.c_str(), full.c_str()) != 0) {
        // Cross-device or missing target: fall back to skipping (like
        // libarchive's WARN path) rather than failing the whole job.
      } else {
        ++files;
      }
      publish();
      continue;
    }
    if (typeflag == '0' || typeflag == '\0') {
      if (!ensure_parent(full)) return false;
      if (!copy_entry_data(src, full, size, prog, false)) return false;
      uint64_t pad = (512 - (size % 512)) % 512;
      if (pad) {
        char pb[512];
        if (!src.read_exact(pb, pad)) return false;
      }
      if (mode) ::chmod(full.c_str(), static_cast<mode_t>(mode & 07777));
      set_mtime(full, static_cast<int64_t>(mtime));
      ++files;
      publish();
      continue;
    }
    // Unknown typeflag (devices, fifos, GNU volumes...): skip data.
    {
      uint64_t pad = (512 - (size % 512)) % 512;
      char tmp[65536];
      uint64_t rem = size;
      while (rem > 0) {
        size_t k = static_cast<size_t>(std::min<uint64_t>(rem, sizeof(tmp)));
        if (!src.read_exact(tmp, k)) return false;
        rem -= k;
      }
      if (pad) {
        char pb[512];
        if (!src.read_exact(pb, pad)) return false;
      }
      publish();
      continue;
    }
  }
}

// ── plain tar / tar.gz / tar.xz dispatch ──────────────────────────────

bool extract_tar_family(const std::string& archive, const std::string& dest,
                        const std::shared_ptr<OperationProgress>& prog,
                        int kind) {  // 0=tar 1=gz 2=xz 3=bz2
  struct stat st{};
  if (::stat(archive.c_str(), &st) != 0) return false;
  uint64_t total = static_cast<uint64_t>(st.st_size);
  FILE* f = std::fopen(archive.c_str(), "rb");
  if (!f) return false;
  std::string dest_abs = fs::absolute(dest).lexically_normal().string();
  prog->total_files.store(0);
  prog->total_bytes.store(0);
  prog->start_time = std::chrono::steady_clock::now();
  bool ok = false;
  if (kind == 0) {
    FileSource src(f, total, prog);
    ok = extract_tar_stream(src, dest, prog, dest_abs);
  }
#ifdef EH_HAVE_ZLIB
  else if (kind == 1) {
    GzipSource src(f, total, prog);
    ok = extract_tar_stream(src, dest, prog, dest_abs);
  }
#endif
#ifdef EH_HAVE_LZMA
  else if (kind == 2) {
    XzSource src(f, total, prog);
    ok = extract_tar_stream(src, dest, prog, dest_abs);
  }
#endif
#ifdef EH_HAVE_BZ2
  else if (kind == 3) {
    Bz2Source src(f, total, prog);
    ok = extract_tar_stream(src, dest, prog, dest_abs);
  }
#endif
  else {
    std::fclose(f);
    return false;
  }
  std::fclose(f);
  return ok && !prog->cancel.load();
}

// ── zip ───────────────────────────────────────────────────────────────

#ifdef EH_HAVE_ZLIB
uint16_t rd16(const unsigned char* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) |
                               (p[3] << 24));
}
uint64_t rd64(const unsigned char* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

void dos_to_unix(uint16_t dos_time, uint16_t dos_date, int64_t& out) {
  int sec = (dos_time & 0x1f) * 2;
  int min = (dos_time >> 5) & 0x3f;
  int hour = (dos_time >> 11) & 0x1f;
  int day = dos_date & 0x1f;
  int mon = ((dos_date >> 5) & 0x0f);
  int year = ((dos_date >> 9) & 0x7f) + 1980;
  if (year < 1980 || mon < 1 || mon > 12 || day < 1 || day > 31) {
    out = 0;
    return;
  }
  std::tm tmv{};
  tmv.tm_sec = sec;
  tmv.tm_min = min;
  tmv.tm_hour = hour;
  tmv.tm_mday = day;
  tmv.tm_mon = mon - 1;
  tmv.tm_year = year - 1900;
  tmv.tm_isdst = -1;
  out = static_cast<int64_t>(std::mktime(&tmv));
}

// Scan extra fields for zip64 sizes and Info-ZIP UT timestamp.
void parse_zip_extra(const unsigned char* extra, size_t len, bool is_central,
                     uint64_t& uncomp, uint64_t& comp, uint64_t& offset,
                     int64_t& mtime, bool& has_mtime) {
  size_t i = 0;
  while (i + 4 <= len) {
    uint16_t id = rd16(extra + i);
    uint16_t sz = rd16(extra + i + 2);
    i += 4;
    if (i + sz > len) break;
    if (id == 0x0001) {  // zip64
      size_t k = 0;
      if (!is_central) {
        // Local: only fields with 0xFFFFFFFF placeholders appear, in
        // order uncomp/comp. (Offset never appears locally.)
      } else {
        if (uncomp == 0xFFFFFFFFu && k + 8 <= sz) {
          uncomp = rd64(extra + i + k);
          k += 8;
        }
        if (comp == 0xFFFFFFFFu && k + 8 <= sz) {
          comp = rd64(extra + i + k);
          k += 8;
        }
        if (offset == 0xFFFFFFFFu && k + 8 <= sz) {
          offset = rd64(extra + i + k);
          k += 8;
        }
      }
    } else if (id == 0x5455) {  // extended timestamp
      if (sz >= 1) {
        unsigned flags = extra[i];
        size_t k = 1;
        if ((flags & 1) && k + 4 <= sz) {
          uint32_t mt = rd32(extra + i + k);
          mtime = static_cast<int64_t>(mt);
          has_mtime = true;
        }
      }
    }
    i += sz;
  }
}

struct ZipEntry {
  std::string name;
  uint16_t method = 0;
  uint16_t flags = 0;
  uint32_t crc = 0;
  uint64_t comp = 0;
  uint64_t uncomp = 0;
  uint64_t local_off = 0;
  uint32_t ext_attr = 0;
  int64_t mtime = 0;
};

bool extract_zip(const std::string& archive, const std::string& dest,
                 const std::shared_ptr<OperationProgress>& prog) {
  struct stat st{};
  if (::stat(archive.c_str(), &st) != 0) return false;
  uint64_t fsize = static_cast<uint64_t>(st.st_size);
  if (fsize < 22) return false;
  FILE* f = std::fopen(archive.c_str(), "rb");
  if (!f) return false;

  // Find EOCD in the last 64k + 22 bytes.
  uint64_t scan = std::min<uint64_t>(fsize, 65557 + 22);
  std::string tail;
  tail.resize(static_cast<size_t>(scan));
  if (std::fseek(f, static_cast<long>(fsize - scan), SEEK_SET) != 0) {
    std::fclose(f);
    return false;
  }
  if (std::fread(tail.data(), 1, tail.size(), f) != tail.size()) {
    std::fclose(f);
    return false;
  }
  long eocd_pos = -1;
  for (long i = static_cast<long>(tail.size()) - 22; i >= 0; --i) {
    if (tail[static_cast<size_t>(i)] == 'P' && tail[static_cast<size_t>(i) + 1] == 'K' &&
        tail[static_cast<size_t>(i) + 2] == '\x05' && tail[static_cast<size_t>(i) + 3] == '\x06') {
      eocd_pos = i;
      break;
    }
  }
  if (eocd_pos < 0) {
    std::fclose(f);
    return false;
  }
  const unsigned char* eocd =
      reinterpret_cast<const unsigned char*>(tail.data() + eocd_pos);
  uint16_t disk_n = rd16(eocd + 8);
  uint16_t cd_n = rd16(eocd + 10);
  uint32_t cd_size = rd32(eocd + 12);
  uint32_t cd_off = rd32(eocd + 16);
  (void)disk_n;
  uint64_t cd_offset = cd_off;
  uint64_t cd_bytes = cd_size;

  // Zip64 EOCD locator directly before EOCD?
  if (eocd_pos >= 20 && tail[static_cast<size_t>(eocd_pos) - 20] == 'P' &&
      tail[static_cast<size_t>(eocd_pos) - 19] == 'K' &&
      tail[static_cast<size_t>(eocd_pos) - 18] == '\x06' &&
      tail[static_cast<size_t>(eocd_pos) - 17] == '\x07') {
    const unsigned char* loc =
        reinterpret_cast<const unsigned char*>(tail.data() + eocd_pos - 20);
    uint64_t zip64_off = rd64(loc + 8);
    if (std::fseek(f, static_cast<long>(zip64_off), SEEK_SET) == 0) {
      unsigned char h[56];
      if (std::fread(h, 1, sizeof(h), f) == sizeof(h) && h[0] == 'P' &&
          h[1] == 'K' && h[2] == '\x06' && h[3] == '\x06') {
        (void)rd64(h + 32);  // entry count: we parse until the CD is consumed
        cd_bytes = rd64(h + 40);
        cd_offset = rd64(h + 48);
      }
    }
  } else if (cd_n == 0xFFFF || cd_off == 0xFFFFFFFFu) {
    // Declares zip64 but locator missing/unreadable: let libarchive try.
    std::fclose(f);
    return false;
  }

  if (cd_offset + cd_bytes > fsize) {
    std::fclose(f);
    return false;
  }
  std::string cd;
  cd.resize(static_cast<size_t>(cd_bytes));
  if (std::fseek(f, static_cast<long>(cd_offset), SEEK_SET) != 0 ||
      (cd_bytes > 0 &&
       std::fread(cd.data(), 1, cd.size(), f) != cd.size())) {
    std::fclose(f);
    return false;
  }

  std::vector<ZipEntry> entries;
  size_t p = 0;
  uint64_t total_bytes = 0;
  while (p + 46 <= cd.size()) {
    const unsigned char* h =
        reinterpret_cast<const unsigned char*>(cd.data() + p);
    if (!(h[0] == 'P' && h[1] == 'K' && h[2] == '\x01' && h[3] == '\x02')) break;
    ZipEntry e;
    e.flags = rd16(h + 8);
    e.method = rd16(h + 10);
    uint16_t dt = rd16(h + 12), dd = rd16(h + 14);
    dos_to_unix(dt, dd, e.mtime);
    e.crc = rd32(h + 16);
    uint32_t c32 = rd32(h + 20), u32 = rd32(h + 24);
    e.comp = c32;
    e.uncomp = u32;
    uint16_t nlen = rd16(h + 28), elen = rd16(h + 30), clen = rd16(h + 32);
    uint32_t loff = rd32(h + 42);
    e.local_off = loff;
    e.ext_attr = rd32(h + 38);
    if (p + 46 + nlen + elen + clen > cd.size()) {
      std::fclose(f);
      return false;
    }
    e.name.assign(cd.data() + p + 46, nlen);
    int64_t mt = 0;
    bool has_mt = false;
    parse_zip_extra(reinterpret_cast<const unsigned char*>(cd.data() + p + 46 + nlen),
                    elen, true, e.uncomp, e.comp, e.local_off, mt, has_mt);
    if (has_mt) e.mtime = mt;
    if ((e.flags & 1) != 0) {
      // Encrypted: our engine has no crypto; fall back for password flow.
      std::fclose(f);
      return false;
    }
    if (e.method != 0 && e.method != 8) {
      std::fclose(f);
      return false;
    }
    p += 46 + nlen + elen + clen;
    if (!(e.uncomp == 0 && e.comp == 0 && !e.name.empty() && e.name.back() == '/')) {
      if (e.uncomp > 0) {
        // Overflow-safe accumulate; garbage sizes fail closed below.
        if (total_bytes + e.uncomp < total_bytes) {
          std::fclose(f);
          return false;
        }
        total_bytes += e.uncomp;
      }
    }
    entries.push_back(std::move(e));
  }

  std::string dest_abs = fs::absolute(dest).lexically_normal().string();
  prog->total_files.store(static_cast<int>(entries.size()));
  prog->total_bytes.store(total_bytes);
  prog->start_time = std::chrono::steady_clock::now();
  int done_count = 0;

  std::string comp_buf;
  std::string out_buf;
  for (auto& e : entries) {
    if (prog->cancel.load()) {
      std::fclose(f);
      return false;
    }
    prog->set_current_file(short_base(e.name));
    std::string clean = sanitize(e.name);
    bool is_dir = !e.name.empty() && e.name.back() == '/';
    uint32_t mode_hi = e.ext_attr >> 16;
    bool is_symlink = ((mode_hi & 0170000u) == 0120000u);
    if (clean.empty()) {
      ++done_count;
      prog->copied_files.store(done_count);
      continue;
    }
    std::string full = dest_abs + "/" + clean;
    if (!inside_dest(dest_abs, full)) {
      ++done_count;
      prog->copied_files.store(done_count);
      continue;
    }
    if (is_dir) {
      std::error_code ec;
      fs::create_directories(full, ec);
      if (ec) {
        std::fclose(f);
        return false;
      }
      unsigned m = mode_hi & 07777u;
      if (m) ::chmod(full.c_str(), static_cast<mode_t>(m));
      set_mtime(full, e.mtime);
      ++done_count;
      prog->copied_files.store(done_count);
      double fr = entries.empty() ? 0 : static_cast<double>(done_count) / entries.size();
      if (fr > 0.99) fr = 0.99;
      prog->progress.store(fr);
      continue;
    }
    // Read local header to find data offset.
    if (e.local_off + 30 > fsize) {
      std::fclose(f);
      return false;
    }
    if (std::fseek(f, static_cast<long>(e.local_off), SEEK_SET) != 0) {
      std::fclose(f);
      return false;
    }
    unsigned char lh[30];
    if (std::fread(lh, 1, sizeof(lh), f) != sizeof(lh)) {
      std::fclose(f);
      return false;
    }
    if (!(lh[0] == 'P' && lh[1] == 'K' && lh[2] == '\x03' && lh[3] == '\x04')) {
      std::fclose(f);
      return false;
    }
    uint16_t lflags = rd16(lh + 8);
    uint16_t lmethod = rd16(lh + 10);
    uint16_t lnlen = rd16(lh + 26), lelen = rd16(lh + 28);
    if (std::fseek(f, static_cast<long>(e.local_off + 30 + lnlen + lelen),
                   SEEK_SET) != 0) {
      std::fclose(f);
      return false;
    }
    (void)lflags;
    (void)lmethod;
    uint64_t data_off = e.local_off + 30 + lnlen + lelen;
    if (data_off + e.comp > fsize) {
      std::fclose(f);
      return false;
    }
    if (!ensure_parent(full)) {
      std::fclose(f);
      return false;
    }
    if (e.method == 0) {
      // Stored.
      if (is_symlink) {
        comp_buf.resize(static_cast<size_t>(e.comp));
        if (e.comp > 0 &&
            std::fread(comp_buf.data(), 1, static_cast<size_t>(e.comp), f) !=
                static_cast<size_t>(e.comp)) {
          std::fclose(f);
          return false;
        }
        uLong crc = crc_run(crc32(0L, Z_NULL, 0), comp_buf.data(), comp_buf.size());
        if (static_cast<uint32_t>(crc) != e.crc) {
          std::fclose(f);
          return false;
        }
        ::unlink(full.c_str());
        if (::symlink(comp_buf.c_str(), full.c_str()) != 0) {
          std::fclose(f);
          return false;
        }
        prog->done_bytes.fetch_add(e.uncomp);
      } else {
        FILE* o = std::fopen(full.c_str(), "wb");
        if (!o) {
          std::fclose(f);
          return false;
        }
        uLong crc = crc32(0L, Z_NULL, 0);
        uint64_t rem = e.comp;
        char tmp[65536];
        bool wfail = false;
        while (rem > 0) {
          if (prog->cancel.load()) {
            std::fclose(o);
            std::fclose(f);
            return false;
          }
          size_t k = static_cast<size_t>(std::min<uint64_t>(rem, sizeof(tmp)));
          if (std::fread(tmp, 1, k, f) != k) {
            wfail = true;
            break;
          }
          crc = crc_run(crc, tmp, k);
          if (std::fwrite(tmp, 1, k, o) != k) {
            wfail = true;
            break;
          }
          rem -= k;
        }
        std::fclose(o);
        if (wfail || static_cast<uint32_t>(crc) != e.crc) {
          std::fclose(f);
          return false;
        }
        prog->done_bytes.fetch_add(e.uncomp);
        unsigned m = mode_hi & 07777u;
        if (m) ::chmod(full.c_str(), static_cast<mode_t>(m));
        else ::chmod(full.c_str(), 0644);
        set_mtime(full, e.mtime);
      }
    } else {
      // Deflate (raw).
      comp_buf.resize(static_cast<size_t>(e.comp));
      if (e.comp > 0 &&
          std::fread(comp_buf.data(), 1, static_cast<size_t>(e.comp), f) !=
              static_cast<size_t>(e.comp)) {
        std::fclose(f);
        return false;
      }
      out_buf.clear();
      if (e.uncomp > 0) {
        if (e.uncomp > (64ull << 30)) {
          std::fclose(f);
          return false;  // absurd size: fail closed
        }
        out_buf.resize(static_cast<size_t>(e.uncomp));
      }
      z_stream zs{};
      if (inflateInit2(&zs, -15) != Z_OK) {
        std::fclose(f);
        return false;
      }
      zs.next_in = reinterpret_cast<Bytef*>(
          e.comp ? const_cast<char*>(comp_buf.data()) : nullptr);
      zs.avail_in = static_cast<uInt>(e.comp);
      zs.next_out = reinterpret_cast<Bytef*>(
          e.uncomp ? out_buf.data() : nullptr);
      zs.avail_out = static_cast<uInt>(e.uncomp);
      int r = inflate(&zs, Z_FINISH);
      bool zend = (r == Z_STREAM_END);
      // Must consume all input and fill all output for a valid member.
      bool okm = zend && zs.avail_in == 0 && zs.avail_out == 0;
      inflateEnd(&zs);
      if (!okm) {
        std::fclose(f);
        return false;
      }
      uLong crc = crc_run(crc32(0L, Z_NULL, 0), out_buf.data(), out_buf.size());
      if (static_cast<uint32_t>(crc) != e.crc) {
        std::fclose(f);
        return false;
      }
      if (is_symlink) {
        ::unlink(full.c_str());
        std::string tgt(out_buf.begin(), out_buf.end());
        if (::symlink(tgt.c_str(), full.c_str()) != 0) {
          std::fclose(f);
          return false;
        }
        prog->done_bytes.fetch_add(e.uncomp);
      } else {
        FILE* o = std::fopen(full.c_str(), "wb");
        if (!o) {
          std::fclose(f);
          return false;
        }
        if (!out_buf.empty() &&
            std::fwrite(out_buf.data(), 1, out_buf.size(), o) != out_buf.size()) {
          std::fclose(o);
          std::fclose(f);
          return false;
        }
        std::fclose(o);
        prog->done_bytes.fetch_add(e.uncomp);
        unsigned m = mode_hi & 07777u;
        if (m) ::chmod(full.c_str(), static_cast<mode_t>(m));
        else ::chmod(full.c_str(), 0644);
        set_mtime(full, e.mtime);
      }
    }
    // Skip data descriptor if present (flag bit 3): 12 bytes (+4 signature)
    // — file position already accounts via central comp size, but foreign
    // writers with bit 3 and mismatched central sizes would desync; our
    // writer always fills central sizes, and we verified comp bytes above,
    // so nothing to do.
    ++done_count;
    prog->copied_files.store(done_count);
    double fr = entries.empty() ? 0 : static_cast<double>(done_count) / entries.size();
    if (fr > 0.99) fr = 0.99;
    prog->progress.store(fr);
  }
  std::fclose(f);
  if (prog->cancel.load()) return false;
  prog->progress.store(1.0);
  return true;
}
#endif

// ── 7z (our writer's subset) ──────────────────────────────────────────

#ifdef EH_HAVE_LZMA
// 7z variable-length number decoder (matches sz_u64 in engine_7z.cpp).
uint64_t sz_num(const unsigned char*& p, const unsigned char* end, bool& ok) {
  if (p >= end) {
    ok = false;
    return 0;
  }
  unsigned char b = *p++;
  if ((b & 0x80) == 0) return b;
  int n = 1;
  while (n < 8 && (b & (0x80 >> n))) ++n;
  // n = number of trailing bytes (1..7) or 8 for full-wide.
  if (n == 8) {
    if (b != 0xFF || p + 8 > end) {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    for (int j = 0; j < 8; ++j) v |= static_cast<uint64_t>(p[j]) << (8 * j);
    p += 8;
    return v;
  }
  unsigned mask = (0x100 >> (n + 1)) - 1;  // payload bits in first byte
  uint64_t v = b & mask;
  if (p + n > end) {
    ok = false;
    return 0;
  }
  // Per spec: value = (first_payload << (8*n)) | LE(trailing n bytes).
  uint64_t lo = 0;
  for (int j = 0; j < n; ++j) lo |= static_cast<uint64_t>(p[j]) << (8 * j);
  p += n;
  v = (v << (8 * n)) | lo;
  return v;
}

std::string sz_utf16le_to_utf8(const unsigned char* p, size_t nbytes) {
  std::string out;
  for (size_t i = 0; i + 1 < nbytes;) {
    uint32_t lo = static_cast<uint32_t>(p[i] | (p[i + 1] << 8));
    i += 2;
    uint32_t cp = lo;
    if (lo >= 0xD800 && lo <= 0xDBFF && i + 1 < nbytes) {
      uint32_t hi2 = static_cast<uint32_t>(p[i] | (p[i + 1] << 8));
      if (hi2 >= 0xDC00 && hi2 <= 0xDFFF) {
        cp = 0x10000 + ((lo - 0xD800) << 10) + (hi2 - 0xDC00);
        i += 2;
      }
    }
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

int64_t filetime_to_unix(uint64_t ft) {
  if (ft < 11644473600ull * 10000000ull) return 0;
  return static_cast<int64_t>((ft / 10000000ull) - 11644473600ull);
}

struct SzFolder {
  uint8_t prop = 0;
  uint32_t dict = 0;
  uint64_t unpack_size = 0;
  uint64_t pack_size = 0;
  uint32_t pack_crc = 0;
  uint32_t folder_crc = 0;
  uint64_t pack_off = 0;  // offset within packed area
  std::vector<uint64_t> stream_sizes;  // per unpack substream
  std::vector<uint32_t> stream_crcs;   // only when multi (writer rule)
};

bool lzma_prop_to_opts(uint8_t prop, uint32_t dict, lzma_options_lzma& opt) {
  for (unsigned pb = 0; pb <= 4; ++pb)
    for (unsigned lp = 0; lp <= 4; ++lp)
      for (unsigned lc = 0; lc <= 8; ++lc) {
        if ((((pb * 5 + lp) * 9 + lc) & 0xFF) == prop) {
          if (pb + lp > 4) continue;
          opt.dict_size = dict;
          opt.lc = lc;
          opt.lp = lp;
          opt.pb = pb;
          opt.mode = LZMA_MODE_NORMAL;
          opt.nice_len = 64;
          opt.mf = LZMA_MF_BT4;
          opt.depth = 0;
          return true;
        }
      }
  return false;
}

bool extract_7z(const std::string& archive, const std::string& dest,
                const std::shared_ptr<OperationProgress>& prog) {
  struct stat st{};
  if (::stat(archive.c_str(), &st) != 0) return false;
  uint64_t fsize = static_cast<uint64_t>(st.st_size);
  if (fsize < 32) return false;
  FILE* f = std::fopen(archive.c_str(), "rb");
  if (!f) return false;
  unsigned char sig[32];
  if (std::fread(sig, 1, 32, f) != 32) {
    std::fclose(f);
    return false;
  }
  if (!(sig[0] == 0x37 && sig[1] == 0x7A && sig[2] == 0xBC && sig[3] == 0xAF &&
        sig[4] == 0x27 && sig[5] == 0x1C)) {
    std::fclose(f);
    return false;
  }
  if (sig[6] != 0x00 || sig[7] != 0x04) {
    std::fclose(f);
    return false;
  }
  uint32_t want_start_crc = rd32(sig + 8);
  uint64_t next_off = rd64(sig + 12);
  uint64_t next_size = rd64(sig + 20);
  uint32_t next_crc = rd32(sig + 28);
  {
    uLong c = crc_run(crc32(0L, Z_NULL, 0),
                      reinterpret_cast<const char*>(sig + 12), 20);
    if (static_cast<uint32_t>(c) != want_start_crc) {
      std::fclose(f);
      return false;
    }
  }
  if (32 + next_off + next_size > fsize && !(next_size == 0)) {
    std::fclose(f);
    return false;
  }
  std::string hdr;
  hdr.resize(static_cast<size_t>(next_size));
  if (next_size > 0) {
    if (std::fseek(f, static_cast<long>(32 + next_off), SEEK_SET) != 0 ||
        std::fread(hdr.data(), 1, hdr.size(), f) != hdr.size()) {
      std::fclose(f);
      return false;
    }
    uLong c = crc_run(crc32(0L, Z_NULL, 0), hdr.data(), hdr.size());
    if (static_cast<uint32_t>(c) != next_crc) {
      std::fclose(f);
      return false;
    }
  }
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(hdr.data());
  const unsigned char* end = p + hdr.size();
  bool ok = true;
  auto need = [&](size_t n) {
    if (!ok || p + n > end) {
      ok = false;
      return false;
    }
    return true;
  };
  auto u8 = [&]() -> unsigned {
    if (!need(1)) return 0;
    return *p++;
  };

  if (!need(1) || p[0] != 0x01) {
    std::fclose(f);
    return false;  // kHeader expected; encoded headers fall back
  }
  ++p;

  std::vector<SzFolder> folders;
  std::vector<uint64_t> pack_sizes;
  std::vector<uint32_t> pack_crcs;

  if (p < end && *p == 0x04) {  // kMainStreamsInfo
    ++p;
    if (!need(1) || *p != 0x06) {
      std::fclose(f);
      return false;
    }
    ++p;  // kPackInfo
    uint64_t pack_pos = sz_num(p, end, ok);
    uint64_t nstreams = sz_num(p, end, ok);
    if (!ok || pack_pos != 0) {
      std::fclose(f);
      return false;
    }
    if (!need(1)) {
      std::fclose(f);
      return false;
    }
    if (*p == 0x09) {  // kSize
      ++p;
      pack_sizes.reserve(static_cast<size_t>(nstreams));
      for (uint64_t i = 0; i < nstreams; ++i)
        pack_sizes.push_back(sz_num(p, end, ok));
      if (!ok || !need(1)) {
        std::fclose(f);
        return false;
      }
    }
    if (*p == 0x0A) {  // kCRC
      ++p;
      if (!need(1)) {
        std::fclose(f);
        return false;
      }
      unsigned def = u8();
      if (def == 1) {
        for (uint64_t i = 0; i < nstreams; ++i) {
          if (!need(4)) {
            std::fclose(f);
            return false;
          }
          pack_crcs.push_back(rd32(p));
          p += 4;
        }
      } else if (def != 0) {
        std::fclose(f);
        return false;
      }
    }
    if (!need(1) || *p != 0x00) {
      std::fclose(f);
      return false;
    }
    ++p;  // end PackInfo
    if (!need(1) || *p != 0x07) {
      std::fclose(f);
      return false;
    }
    ++p;  // kUnpackInfo
    if (!need(1) || *p != 0x0B) {
      std::fclose(f);
      return false;
    }
    ++p;  // kFolder
    uint64_t nfolders = sz_num(p, end, ok);
    if (!ok || nfolders > 1000000) {
      std::fclose(f);
      return false;
    }
    if (!need(1)) {
      std::fclose(f);
      return false;
    }
    unsigned ext = u8();
    if (ext != 0) {
      std::fclose(f);
      return false;  // external folders unsupported
    }
    folders.reserve(static_cast<size_t>(nfolders));
    for (uint64_t fi = 0; fi < nfolders; ++fi) {
      uint64_t ncoders = sz_num(p, end, ok);
      if (!ok || ncoders != 1) {
        std::fclose(f);
        return false;
      }
      if (!need(4)) {
        std::fclose(f);
        return false;
      }
      // Writer emits flags 0x23, id len 3, id 0x030101, props size 5.
      if (p[0] != 0x23 || p[1] != 0x03 || p[2] != 0x01 || p[3] != 0x01) {
        std::fclose(f);
        return false;
      }
      p += 4;
      if (!need(1)) {
        std::fclose(f);
        return false;
      }
      unsigned propsz = u8();
      if (propsz != 5 || !need(5)) {
        std::fclose(f);
        return false;
      }
      SzFolder fo;
      fo.prop = p[0];
      fo.dict = static_cast<uint32_t>(p[1] | (p[2] << 8) | (p[3] << 16) |
                                      (p[4] << 24));
      p += 5;
      folders.push_back(fo);
    }
    if (!need(1) || *p != 0x0C) {
      std::fclose(f);
      return false;
    }
    ++p;  // kCodersUnpackSize
    for (auto& fo : folders) fo.unpack_size = sz_num(p, end, ok);
    if (!ok || !need(1)) {
      std::fclose(f);
      return false;
    }
    if (*p != 0x0A) {
      std::fclose(f);
      return false;
    }
    ++p;  // kCRC
    if (!need(1)) {
      std::fclose(f);
      return false;
    }
    if (u8() != 1) {
      std::fclose(f);
      return false;
    }
    for (auto& fo : folders) {
      if (!need(4)) {
        std::fclose(f);
        return false;
      }
      fo.folder_crc = rd32(p);
      p += 4;
    }
    if (!need(1) || *p != 0x00) {
      std::fclose(f);
      return false;
    }
    ++p;  // end UnpackInfo
    // SubStreamsInfo
    if (!need(1) || *p != 0x08) {
      std::fclose(f);
      return false;
    }
    ++p;
    std::vector<uint64_t> nstreams_per;
    nstreams_per.assign(folders.size(), 1);
    if (p < end && *p == 0x0D) {
      ++p;
      for (size_t i = 0; i < folders.size(); ++i)
        nstreams_per[i] = sz_num(p, end, ok);
      if (!ok) {
        std::fclose(f);
        return false;
      }
    }
    // kSize (all but last per multi-stream folder)
    std::vector<std::vector<uint64_t>> sub_sizes(folders.size());
    if (p < end && *p == 0x09) {
      ++p;
      for (size_t i = 0; i < folders.size(); ++i) {
        for (uint64_t k = 0; k + 1 < nstreams_per[i]; ++k)
          sub_sizes[i].push_back(sz_num(p, end, ok));
        if (!ok) {
          std::fclose(f);
          return false;
        }
      }
    }
    // kCRC for multi-stream folders
    std::vector<std::vector<uint32_t>> sub_crcs(folders.size());
    if (p < end && *p == 0x0A) {
      ++p;
      if (!need(1)) {
        std::fclose(f);
        return false;
      }
      if (u8() != 1) {
        std::fclose(f);
        return false;
      }
      for (size_t i = 0; i < folders.size(); ++i) {
        if (nstreams_per[i] == 1) continue;
        for (uint64_t k = 0; k < nstreams_per[i]; ++k) {
          if (!need(4)) {
            std::fclose(f);
            return false;
          }
          sub_crcs[i].push_back(rd32(p));
          p += 4;
        }
      }
    }
    if (!need(1) || *p != 0x00) {
      std::fclose(f);
      return false;
    }
    ++p;  // end SubStreamsInfo
    if (!need(1) || *p != 0x00) {
      std::fclose(f);
      return false;
    }
    ++p;  // end MainStreamsInfo
    // Attach pack stream metadata + substream sizes to folders.
    if (pack_sizes.size() != folders.size()) {
      std::fclose(f);
      return false;
    }
    uint64_t off = 0;
    for (size_t i = 0; i < folders.size(); ++i) {
      folders[i].pack_size = pack_sizes[i];
      folders[i].pack_off = off;
      off += pack_sizes[i];
      if (i < pack_crcs.size()) folders[i].pack_crc = pack_crcs[i];
      // Reconstruct full per-stream sizes: stored all-but-last; last is
      // unpack_size - sum(stored).
      if (nstreams_per[i] == 1) {
        folders[i].stream_sizes.push_back(folders[i].unpack_size);
      } else {
        uint64_t sum = 0;
        for (auto s : sub_sizes[i]) sum += s;
        if (sum > folders[i].unpack_size) {
          std::fclose(f);
          return false;
        }
        folders[i].stream_sizes = sub_sizes[i];
        folders[i].stream_sizes.push_back(folders[i].unpack_size - sum);
        folders[i].stream_crcs = sub_crcs[i];
      }
    }
  }

  // FilesInfo
  if (!need(1) || *p != 0x05) {
    std::fclose(f);
    return false;
  }
  ++p;
  uint64_t nfiles = sz_num(p, end, ok);
  if (!ok || nfiles > 10000000) {
    std::fclose(f);
    return false;
  }
  std::vector<char> is_empty(static_cast<size_t>(nfiles), 0);
  if (p < end && *p == 0x0E) {
    ++p;
    uint64_t msz = sz_num(p, end, ok);
    if (!ok || !need(static_cast<size_t>(msz))) {
      std::fclose(f);
      return false;
    }
    for (uint64_t i = 0; i < nfiles; ++i) {
      size_t byte = static_cast<size_t>(i / 8);
      int bit = 7 - static_cast<int>(i % 8);
      if (byte < msz) is_empty[static_cast<size_t>(i)] = (p[byte] >> bit) & 1;
    }
    p += msz;
  }
  std::vector<char> is_empty_file;
  is_empty_file.assign(static_cast<size_t>(nfiles), 0);
  if (p < end && *p == 0x0F) {
    ++p;
    uint64_t msz = sz_num(p, end, ok);
    if (!ok || !need(static_cast<size_t>(msz))) {
      std::fclose(f);
      return false;
    }
    size_t k = 0;
    for (uint64_t i = 0; i < nfiles; ++i) {
      if (!is_empty[static_cast<size_t>(i)]) continue;
      size_t byte = k / 8;
      int bit = 7 - static_cast<int>(k % 8);
      if (byte < msz)
        is_empty_file[static_cast<size_t>(i)] = (p[byte] >> bit) & 1;
      ++k;
    }
    p += msz;
  }
  std::vector<std::string> names;
  if (p < end && *p == 0x11) {
    ++p;
    uint64_t bsz = sz_num(p, end, ok);
    if (!ok || !need(1)) {
      std::fclose(f);
      return false;
    }
    if (u8() != 0) {
      std::fclose(f);
      return false;  // only inline names supported
    }
    if (!need(static_cast<size_t>(bsz - 1))) {
      std::fclose(f);
      return false;
    }
    const unsigned char* bp = p;
    const unsigned char* bend = p + (bsz - 1);
    for (uint64_t i = 0; i < nfiles; ++i) {
      const unsigned char* zs = bp;
      while (bend - bp >= 2 && !(bp[0] == 0 && bp[1] == 0)) bp += 2;
      if (bend - bp < 2) {
        std::fclose(f);
        return false;
      }
      names.push_back(
          sz_utf16le_to_utf8(zs, static_cast<size_t>(bp - zs)));
      bp += 2;
    }
    p += (bsz - 1);
  } else {
    std::fclose(f);
    return false;
  }
  std::vector<int64_t> mtimes(static_cast<size_t>(nfiles), 0);
  std::vector<uint32_t> attrs(static_cast<size_t>(nfiles), 0);
  while (p < end && *p != 0x00) {
    unsigned id = u8();
    if (id == 0x14) {  // kMTime
      uint64_t sz = sz_num(p, end, ok);
      if (!ok || !need(2)) {
        std::fclose(f);
        return false;
      }
      if (u8() != 1 || u8() != 0) {
        std::fclose(f);
        return false;
      }
      if (sz != 2 + 8 * nfiles || !need(static_cast<size_t>(8 * nfiles))) {
        std::fclose(f);
        return false;
      }
      for (uint64_t i = 0; i < nfiles; ++i) {
        uint64_t ft = rd64(p);
        p += 8;
        mtimes[static_cast<size_t>(i)] = filetime_to_unix(ft);
      }
    } else if (id == 0x15) {  // kWinAttrib
      uint64_t sz = sz_num(p, end, ok);
      if (!ok || !need(2)) {
        std::fclose(f);
        return false;
      }
      if (u8() != 1 || u8() != 0) {
        std::fclose(f);
        return false;
      }
      if (sz != 2 + 4 * nfiles || !need(static_cast<size_t>(4 * nfiles))) {
        std::fclose(f);
        return false;
      }
      for (uint64_t i = 0; i < nfiles; ++i) {
        attrs[static_cast<size_t>(i)] = rd32(p);
        p += 4;
      }
    } else {
      std::fclose(f);
      return false;
    }
  }
  if (!need(1) || *p != 0x00) {
    std::fclose(f);
    return false;
  }
  ++p;  // end FilesInfo
  if (!need(1) || *p != 0x00) {
    std::fclose(f);
    return false;
  }

  // Map non-empty files (in order) to folder streams (in order).
  struct StreamRef {
    size_t folder = 0;
    size_t stream = 0;
  };
  std::vector<StreamRef> file_stream(static_cast<size_t>(nfiles),
                                     {SIZE_MAX, SIZE_MAX});
  {
    size_t fi = 0, si = 0;
    // Build ordered list of (folder, stream) pairs.
    std::vector<StreamRef> all_streams;
    for (size_t b = 0; b < folders.size(); ++b)
      for (size_t s = 0; s < folders[b].stream_sizes.size(); ++s)
        all_streams.push_back({b, s});
    for (uint64_t i = 0; i < nfiles; ++i) {
      if (is_empty[static_cast<size_t>(i)]) continue;
      if (si >= all_streams.size()) {
        std::fclose(f);
        return false;
      }
      file_stream[static_cast<size_t>(i)] = all_streams[si++];
    }
    if (si != all_streams.size()) {
      std::fclose(f);
      return false;
    }
    (void)fi;
  }

  uint64_t total_bytes = 0;
  for (size_t i = 0; i < folders.size(); ++i)
    total_bytes += folders[i].unpack_size;
  prog->total_files.store(static_cast<int>(nfiles));
  prog->total_bytes.store(total_bytes);
  prog->start_time = std::chrono::steady_clock::now();

  // Decode each folder's packed stream.
  std::vector<std::string> folder_data(folders.size());
  for (size_t b = 0; b < folders.size(); ++b) {
    if (prog->cancel.load()) {
      std::fclose(f);
      return false;
    }
    SzFolder& fo = folders[b];
    std::string pack;
    pack.resize(static_cast<size_t>(fo.pack_size));
    if (std::fseek(f, static_cast<long>(32 + fo.pack_off), SEEK_SET) != 0 ||
        (fo.pack_size > 0 &&
         std::fread(pack.data(), 1, pack.size(), f) != pack.size())) {
      std::fclose(f);
      return false;
    }
    if (!pack_crcs.empty()) {
      uLong c = crc_run(crc32(0L, Z_NULL, 0), pack.data(), pack.size());
      if (static_cast<uint32_t>(c) != fo.pack_crc) {
        std::fclose(f);
        return false;
      }
    }
    lzma_options_lzma opt{};
    if (!lzma_prop_to_opts(fo.prop, fo.dict, opt)) {
      std::fclose(f);
      return false;
    }
    lzma_filter filters[2];
    filters[0].id = LZMA_FILTER_LZMA1;
    filters[0].options = &opt;
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = nullptr;
    lzma_stream ds = LZMA_STREAM_INIT;
    if (lzma_raw_decoder(&ds, filters) != LZMA_OK) {
      std::fclose(f);
      return false;
    }
    std::string out;
    out.resize(static_cast<size_t>(fo.unpack_size));
    ds.next_in = reinterpret_cast<const uint8_t*>(pack.data());
    ds.avail_in = pack.size();
    ds.next_out = reinterpret_cast<uint8_t*>(
        fo.unpack_size ? out.data() : nullptr);
    ds.avail_out = static_cast<size_t>(fo.unpack_size);
    lzma_ret r = lzma_code(&ds, LZMA_FINISH);
    bool dend = (r == LZMA_STREAM_END);
    bool dall = (ds.avail_in == 0 && ds.avail_out == 0);
    lzma_end(&ds);
    if (!dend || !dall) {
      std::fclose(f);
      return false;
    }
    uLong fc = crc_run(crc32(0L, Z_NULL, 0), out.data(), out.size());
    if (static_cast<uint32_t>(fc) != fo.folder_crc) {
      std::fclose(f);
      return false;
    }
    folder_data[b] = std::move(out);
  }
  std::fclose(f);

  // Split folders into files and write.
  std::vector<size_t> folder_pos(folders.size(), 0);
  std::string dest_abs = fs::absolute(dest).lexically_normal().string();
  int done_count = 0;
  for (uint64_t i = 0; i < nfiles; ++i) {
    if (prog->cancel.load()) return false;
    std::string rel = (i < names.size()) ? names[static_cast<size_t>(i)] : "";
    prog->set_current_file(short_base(rel));
    std::string clean = sanitize(rel);
    uint32_t attr = attrs[static_cast<size_t>(i)];
    uint32_t umode = attr >> 16;
    uint32_t ftype = umode & 0170000u;
    int64_t mt = mtimes[static_cast<size_t>(i)];
    bool empty = is_empty[static_cast<size_t>(i)] != 0;
    bool empty_is_file = is_empty_file[static_cast<size_t>(i)] != 0;
    if (clean.empty()) {
      ++done_count;
      prog->copied_files.store(done_count);
      continue;
    }
    std::string full = dest_abs + "/" + clean;
    if (!inside_dest(dest_abs, full)) {
      ++done_count;
      prog->copied_files.store(done_count);
      continue;
    }
    if (ftype == 0040000u || (!empty_is_file && empty)) {
      // Directory (writer marks dirs 0040000; empties-only rule keeps the
      // same split for foreign readers).
      std::error_code ec;
      fs::create_directories(full, ec);
      if (ec) return false;
      unsigned m = umode & 07777u;
      if (m) ::chmod(full.c_str(), static_cast<mode_t>(m));
      set_mtime(full, mt);
      ++done_count;
      prog->copied_files.store(done_count);
      continue;
    }
    if (ftype == 0120000u) {
      // Symlink: stream bytes are the target.
      StreamRef sr = file_stream[static_cast<size_t>(i)];
      std::string tgt;
      if (sr.folder != SIZE_MAX) {
        SzFolder& fo = folders[sr.folder];
        uint64_t sz = fo.stream_sizes[sr.stream];
        const std::string& fd = folder_data[sr.folder];
        size_t pos = folder_pos[sr.folder];
        if (pos + sz > fd.size()) return false;
        tgt.assign(fd.data() + pos, static_cast<size_t>(sz));
        folder_pos[sr.folder] += static_cast<size_t>(sz);
        prog->done_bytes.fetch_add(sz);
      }
      if (!ensure_parent(full)) return false;
      ::unlink(full.c_str());
      if (::symlink(tgt.c_str(), full.c_str()) != 0) return false;
      ++done_count;
      prog->copied_files.store(done_count);
      double fr = static_cast<double>(done_count) / static_cast<double>(nfiles);
      if (fr > 0.99) fr = 0.99;
      prog->progress.store(fr);
      continue;
    }
    // Regular file.
    if (empty) {
      if (!ensure_parent(full)) return false;
      FILE* o = std::fopen(full.c_str(), "wb");
      if (!o) return false;
      std::fclose(o);
      unsigned m = umode & 07777u;
      if (m) ::chmod(full.c_str(), static_cast<mode_t>(m));
      set_mtime(full, mt);
      ++done_count;
      prog->copied_files.store(done_count);
      continue;
    }
    StreamRef sr = file_stream[static_cast<size_t>(i)];
    if (sr.folder == SIZE_MAX) return false;
    SzFolder& fo = folders[sr.folder];
    uint64_t sz = fo.stream_sizes[sr.stream];
    const std::string& fd = folder_data[sr.folder];
    size_t pos = folder_pos[sr.folder];
    if (pos + sz > fd.size()) return false;
    if (!ensure_parent(full)) return false;
    FILE* o = std::fopen(full.c_str(), "wb");
    if (!o) return false;
    if (sz > 0 &&
        std::fwrite(fd.data() + pos, 1, static_cast<size_t>(sz), o) !=
            static_cast<size_t>(sz)) {
      std::fclose(o);
      return false;
    }
    std::fclose(o);
    // Per-stream CRC check for multi-stream folders (single-stream folders
    // reuse the folder CRC per the writer's dedup rule).
    if (fo.stream_crcs.size() == fo.stream_sizes.size() && fo.stream_sizes.size() > 1) {
      uLong c = crc_run(crc32(0L, Z_NULL, 0), fd.data() + pos,
                        static_cast<size_t>(sz));
      if (static_cast<uint32_t>(c) != fo.stream_crcs[sr.stream]) return false;
    }
    folder_pos[sr.folder] += static_cast<size_t>(sz);
    prog->done_bytes.fetch_add(sz);
    unsigned m = umode & 07777u;
    if (m) ::chmod(full.c_str(), static_cast<mode_t>(m));
    else ::chmod(full.c_str(), 0644);
    set_mtime(full, mt);
    ++done_count;
    prog->copied_files.store(done_count);
    double fr = static_cast<double>(done_count) / static_cast<double>(nfiles);
    if (fr > 0.99) fr = 0.99;
    prog->progress.store(fr);
  }
  prog->progress.store(1.0);
  prog->copied_files.store(static_cast<int>(nfiles));
  prog->done_bytes.store(total_bytes);
  return true;
}
#endif

}  // namespace

bool engine_supports_extract(const std::string& archive_path) {
  std::string l = lower_copy(archive_path);
  if (ends_with(l, ".tar.gz") || ends_with(l, ".tgz")) {
#ifdef EH_HAVE_ZLIB
    return true;
#else
    return false;
#endif
  }
  if (ends_with(l, ".tar.bz2") || ends_with(l, ".tbz2") ||
      ends_with(l, ".tbz")) {
#ifdef EH_HAVE_BZ2
    return true;
#else
    return false;
#endif
  }
  if (ends_with(l, ".tar.xz") || ends_with(l, ".txz")) {
#ifdef EH_HAVE_LZMA
    return true;
#else
    return false;
#endif
  }
  if (ends_with(l, ".zip")) {
#ifdef EH_HAVE_ZLIB
    return true;
#else
    return false;
#endif
  }
  if (ends_with(l, ".7z")) {
#ifdef EH_HAVE_LZMA
    return true;
#else
    return false;
#endif
  }
  if (ends_with(l, ".tar")) return true;
  return false;
}

bool engine_extract(const std::string& archive_path,
                    const std::string& dest_dir,
                    const std::shared_ptr<OperationProgress>& prog) {
  std::string l = lower_copy(archive_path);
  std::error_code ec;
  fs::create_directories(dest_dir, ec);
  std::string dest_abs = fs::absolute(dest_dir).lexically_normal().string();
  (void)dest_abs;
  if (ends_with(l, ".tar.gz") || ends_with(l, ".tgz")) {
#ifdef EH_HAVE_ZLIB
    return extract_tar_family(archive_path, dest_dir, prog, 1);
#else
    return false;
#endif
  }
  if (ends_with(l, ".tar.bz2") || ends_with(l, ".tbz2") ||
      ends_with(l, ".tbz")) {
#ifdef EH_HAVE_BZ2
    return extract_tar_family(archive_path, dest_dir, prog, 3);
#else
    return false;
#endif
  }
  if (ends_with(l, ".tar.xz") || ends_with(l, ".txz")) {
#ifdef EH_HAVE_LZMA
    return extract_tar_family(archive_path, dest_dir, prog, 2);
#else
    return false;
#endif
  }
  if (ends_with(l, ".zip")) {
#ifdef EH_HAVE_ZLIB
    return extract_zip(archive_path, dest_dir, prog);
#else
    return false;
#endif
  }
  if (ends_with(l, ".7z")) {
#ifdef EH_HAVE_LZMA
    return extract_7z(archive_path, dest_dir, prog);
#else
    return false;
#endif
  }
  if (ends_with(l, ".tar")) return extract_tar_family(archive_path, dest_dir, prog, 0);
  return false;
}

bool engine_archive_is_encrypted(const std::string& archive_path) {
  std::string l = lower_copy(archive_path);
  FILE* f = std::fopen(archive_path.c_str(), "rb");
  if (!f) return false;
  struct stat st{};
  bool enc = false;
  if (::fstat(::fileno(f), &st) != 0) {
    std::fclose(f);
    return false;
  }
  uint64_t fsize = static_cast<uint64_t>(st.st_size);
  if (ends_with(l, ".zip")) {
#ifdef EH_HAVE_ZLIB
    // Scan central directory flags for bit 0 (encryption). Reuses the same
    // EOCD walk as extract_zip but stops at the first encrypted entry.
    if (fsize >= 22) {
      uint64_t scan = std::min<uint64_t>(fsize, 65557 + 22);
      std::string tail;
      tail.resize(static_cast<size_t>(scan));
      if (std::fseek(f, static_cast<long>(fsize - scan), SEEK_SET) == 0 &&
          std::fread(tail.data(), 1, tail.size(), f) == tail.size()) {
        long eocd_pos = -1;
        for (long i = static_cast<long>(tail.size()) - 22; i >= 0; --i) {
          if (tail[static_cast<size_t>(i)] == 'P' &&
              tail[static_cast<size_t>(i) + 1] == 'K' &&
              tail[static_cast<size_t>(i) + 2] == '\x05' &&
              tail[static_cast<size_t>(i) + 3] == '\x06') {
            eocd_pos = i;
            break;
          }
        }
        if (eocd_pos >= 0) {
          const unsigned char* eocd =
              reinterpret_cast<const unsigned char*>(tail.data() + eocd_pos);
          uint64_t cd_count = rd16(eocd + 10);
          uint64_t cd_offset = rd32(eocd + 16);
          uint64_t cd_bytes = rd32(eocd + 12);
          if (cd_offset + cd_bytes <= fsize && cd_bytes > 0 &&
              cd_bytes < (16ull << 20)) {
            std::string cd;
            cd.resize(static_cast<size_t>(cd_bytes));
            if (std::fseek(f, static_cast<long>(cd_offset), SEEK_SET) == 0 &&
                std::fread(cd.data(), 1, cd.size(), f) == cd.size()) {
              size_t p = 0;
              for (uint64_t k = 0; k < cd_count && p + 46 <= cd.size(); ++k) {
                const unsigned char* h =
                    reinterpret_cast<const unsigned char*>(cd.data() + p);
                if (!(h[0] == 'P' && h[1] == 'K' && h[2] == '\x01' &&
                      h[3] == '\x02'))
                  break;
                if ((rd16(h + 8) & 1) != 0) {
                  enc = true;
                  break;
                }
                uint16_t nlen = rd16(h + 28), elen = rd16(h + 30),
                         clen = rd16(h + 32);
                if (p + 46 + nlen + elen + clen > cd.size()) break;
                p += 46 + nlen + elen + clen;
              }
            }
          }
        }
      }
    }
#endif
  } else if (ends_with(l, ".7z")) {
#ifdef EH_HAVE_LZMA
    // Our writer emits a plain kHeader (0x01). An encoded header (0x17)
    // means AES/encoded streams -> password required.
    if (fsize >= 32 + 1) {
      unsigned char sig[32];
      if (std::fseek(f, 0, SEEK_SET) == 0 &&
          std::fread(sig, 1, 32, f) == 32 &&
          sig[0] == 0x37 && sig[1] == 0x7A && sig[2] == 0xBC &&
          sig[3] == 0xAF && sig[4] == 0x27 && sig[5] == 0x1C) {
        uint64_t next_off = rd64(sig + 12);
        // Peek first header byte without CRC-verifying (probe only).
        if (std::fseek(f, static_cast<long>(32 + next_off), SEEK_SET) == 0) {
          int b = std::fgetc(f);
          if (b == 0x17) enc = true;  // kEncodedHeader
        }
      } else {
        enc = false;
      }
    }
#endif
  } else {
    enc = false;  // tar family has no encryption marker
  }
  std::fclose(f);
  return enc;
}

}  // namespace eh::file_browser
