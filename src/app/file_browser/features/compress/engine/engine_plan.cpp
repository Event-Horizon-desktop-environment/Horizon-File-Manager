// Split from compress_engine.cpp: one format per translation unit.
#include "app/file_browser/features/compress/compress_engine.hpp"
#include "app/file_browser/features/compress/engine/engine_internal.hpp"
#include "app/file_browser/features/compress/compress.hpp"
#include "app/file_browser/features/progress/progress.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace eh::file_browser {


// 512-byte ustar header with pax 'x' fallback for long names and base-256
// for huge sizes. Appends header block(s) for one entry to `out`.
void put_octal(char* dst, size_t w, uint64_t v) {
  // w includes the trailing NUL. Overflow -> base-256 binary, leading 0x80.
  char tmp[32];
  std::snprintf(tmp, sizeof(tmp), "%0*llo", static_cast<int>(w - 1),
                static_cast<unsigned long long>(v));
  size_t n = std::strlen(tmp);
  if (n >= w) {
    dst[0] = static_cast<char>(0x80);
    for (size_t i = 1; i < w; ++i) dst[i] = 0;
    for (size_t i = 0; i < sizeof(v) && i < w - 1; ++i) {
      dst[w - 1 - i] = static_cast<char>(v & 0xff);
      v >>= 8;
    }
    return;
  }
  std::memcpy(dst, tmp + (n > w - 1 ? n - (w - 1) : 0), w - 1);
  dst[w - 1] = '\0';
}

void append_tar_header(std::string& out, const std::string& name, uint64_t size,
                       unsigned mode, int64_t mtime, int uid, int gid,
                       char typeflag, const std::string& linkname) {
  // pax extended header when name/linkname exceed ustar limits.
  std::string pax;
  auto pax_rec = [&](const char* key, const std::string& val) {
    // Record "LEN key=val\n" where LEN counts itself: LEN = digits(LEN) + body.
    std::string body = std::string(" ") + key + "=" + val + "\n";
    size_t t = body.size() + 1;
    for (;;) {
      size_t nt = body.size() + std::to_string(t).size();
      if (nt == t) break;
      t = nt;
    }
    pax += std::to_string(t) + body;
  };
  std::string short_name = name, short_link = linkname;
  if (name.size() > 100) {
    pax_rec("path", name);
    short_name = name.substr(0, 100);
  }
  if (!linkname.empty() && linkname.size() > 100) {
    pax_rec("linkpath", linkname);
    short_link = linkname.substr(0, 100);
  }
  if (!pax.empty()) {
    char hb[512] = {};
    std::memcpy(hb, "././@PaxHeader", 14);
    put_octal(hb + 100, 8, 0644);
    put_octal(hb + 108, 8, 0);
    put_octal(hb + 116, 8, 0);
    put_octal(hb + 124, 12, pax.size());
    put_octal(hb + 136, 12, mtime);
    hb[156] = 'x';
    std::memcpy(hb + 257, "ustar", 5);
    hb[263] = hb[264] = '0';
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(hb[i]);
    std::snprintf(hb + 148, 8, "%06o", sum);
    hb[155] = ' ';
    out.append(hb, 512);
    out += pax;
    out.append((512 - pax.size() % 512) % 512, '\0');
  }
  char hb[512] = {};
  std::memcpy(hb, short_name.c_str(), std::min(short_name.size(), size_t(100)));
  put_octal(hb + 100, 8, mode & 07777);
  put_octal(hb + 108, 8, static_cast<uint64_t>(uid));
  put_octal(hb + 116, 8, static_cast<uint64_t>(gid));
  put_octal(hb + 124, 12, size);
  put_octal(hb + 136, 12, static_cast<uint64_t>(mtime));
  hb[156] = typeflag;
  if (!short_link.empty())
    std::memcpy(hb + 157, short_link.c_str(), std::min(short_link.size(), size_t(100)));
  std::memcpy(hb + 257, "ustar", 5);
  hb[263] = hb[264] = '0';
  unsigned sum = 0;
  for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(hb[i]);
  std::snprintf(hb + 148, 8, "%06o", sum);
  hb[155] = ' ';
  out.append(hb, 512);
}


#if defined(EH_HAVE_ZLIB) || defined(EH_HAVE_LZMA)
// CRC32 over an arbitrary-length run (chunked for huge inputs).
uLong crc_run(uLong crc, const char* data, size_t len) {
  while (len > 0) {
    uInt step = static_cast<uInt>(std::min(len, size_t(1u << 30)));
    crc = crc32(crc, reinterpret_cast<const Bytef*>(data), step);
    data += step;
    len -= step;
  }
  return crc;
}
#endif

EnginePlan engine_plan_sources(const std::vector<std::string>& paths,
                               const std::string& parent) {
  EnginePlan plan;
  std::string prefix = parent;
  if (!prefix.empty() && prefix.back() != '/') prefix += '/';
  auto rel_of = [&](const std::string& full) {
    if (full.size() > prefix.size() && full.compare(0, prefix.size(), prefix) == 0)
      return full.substr(prefix.size());
    return fs::path(full).filename().string();
  };
  std::error_code ec;
  for (const auto& p : paths) {
    std::string full = fs::absolute(p).lexically_normal().string();
    struct stat st{};
    if (::lstat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      EngineEntry dir;
      dir.rel = rel_of(full);
      dir.full = full;
      dir.kind = 'd';
      dir.mode = static_cast<unsigned>(st.st_mode & 07777);
      dir.mtime = static_cast<int64_t>(st.st_mtime);
      dir.uid = static_cast<int>(st.st_uid);
      dir.gid = static_cast<int>(st.st_gid);
      plan.entries.push_back(std::move(dir));
      for (auto& de : fs::recursive_directory_iterator(p, ec)) {
        struct stat cst{};
        if (::lstat(de.path().c_str(), &cst) != 0) continue;
        if (!S_ISREG(cst.st_mode) && !S_ISDIR(cst.st_mode) && !S_ISLNK(cst.st_mode))
          continue; // fifos/sockets/devices: skip like tar --warning
        std::string cfull = fs::absolute(de.path().string()).lexically_normal().string();
        EngineEntry e;
        e.rel = rel_of(cfull);
        e.full = cfull;
        e.mode = static_cast<unsigned>(cst.st_mode & 07777);
        e.mtime = static_cast<int64_t>(cst.st_mtime);
        e.uid = static_cast<int>(cst.st_uid);
        e.gid = static_cast<int>(cst.st_gid);
        if (S_ISDIR(cst.st_mode)) {
          e.kind = 'd';
        } else if (S_ISLNK(cst.st_mode)) {
          e.kind = 'l';
          char buf[4096];
          ssize_t n = ::readlink(cfull.c_str(), buf, sizeof(buf) - 1);
          if (n > 0) e.target.assign(buf, static_cast<size_t>(n));
        } else {
          e.kind = 'f';
          e.size = static_cast<uint64_t>(cst.st_size);
          plan.total_bytes += e.size;
          ++plan.file_count;
        }
        plan.entries.push_back(std::move(e));
      }
    } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
      EngineEntry e;
      e.rel = rel_of(full);
      e.full = full;
      e.mode = static_cast<unsigned>(st.st_mode & 07777);
      e.mtime = static_cast<int64_t>(st.st_mtime);
      e.uid = static_cast<int>(st.st_uid);
      e.gid = static_cast<int>(st.st_gid);
      if (S_ISLNK(st.st_mode)) {
        e.kind = 'l';
        char buf[4096];
        ssize_t n = ::readlink(full.c_str(), buf, sizeof(buf) - 1);
        if (n > 0) e.target.assign(buf, static_cast<size_t>(n));
      } else {
        e.kind = 'f';
        e.size = static_cast<uint64_t>(st.st_size);
        plan.total_bytes += e.size;
        ++plan.file_count;
      }
      plan.entries.push_back(std::move(e));
    }
  }
  return plan;
}

} // namespace eh::file_browser
