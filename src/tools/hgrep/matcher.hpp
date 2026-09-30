#pragma once

// matcher.hpp — byte-substring scanning over a buffer.
//
// Exact matching: AVX2 first-byte filter + memcmp verify (3-4.5x glibc
// memmem in microbenchmarks), memchr for 1-byte needles, memmem fallback
// without AVX2. -i uses a case-folded Horspool. All stream
// non-overlapping match offsets through a template callback returning
// false to stop early (no std::function per hit).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define HGREP_HAVE_X86_SIMD 1
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace hgrep {
namespace detail {

inline const char* memmem_find(const char* h, size_t hn, const char* n,
                               size_t nn) {
  if (nn == 0 || nn > hn) return nullptr;
  return static_cast<const char*>(memmem(h, hn, n, nn));
}

struct HorspoolCI {
  std::string needle;
  size_t skip[256];
  explicit HorspoolCI(const std::string& n) {
    needle.resize(n.size());
    for (size_t i = 0; i < n.size(); ++i) {
      unsigned char c = static_cast<unsigned char>(n[i]);
      needle[i] = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    size_t m = needle.size();
    for (int i = 0; i < 256; ++i) skip[i] = m;
    for (size_t i = 0; i + 1 < m; ++i)
      skip[static_cast<unsigned char>(needle[i])] = m - 1 - i;
  }
  const char* find(const char* h, size_t hn) const {
    size_t m = needle.size();
    if (m == 0 || m > hn) return nullptr;
    size_t i = 0;
    while (i <= hn - m) {
      size_t j = m;
      while (j > 0) {
        unsigned char hc = static_cast<unsigned char>(h[i + j - 1]);
        if (hc >= 'A' && hc <= 'Z') hc += 32;
        if (hc != static_cast<unsigned char>(needle[j - 1])) break;
        --j;
      }
      if (j == 0) return h + i;
      unsigned char nx = static_cast<unsigned char>(h[i + m - 1]);
      if (nx >= 'A' && nx <= 'Z') nx += 32;
      i += skip[nx];
    }
    return nullptr;
  }
};

} // namespace detail

#ifdef HGREP_HAVE_X86_SIMD
// AVX2 first-byte filter: 32 lanes per instruction, memcmp only on
// candidate positions. nlen >= 2, len >= 32 (callers guarantee this).
template <typename Cb>
__attribute__((target("avx2"))) size_t
find_all_avx2(const char* data, size_t len, const char* needle, size_t nlen,
              Cb&& cb) {
  const __m256i first = _mm256_set1_epi8(needle[0]);
  size_t count = 0;
  size_t skip_until = 0; // non-overlapping matches (memmem parity)
  const size_t end = len - 32; // last block start (inclusive)
  size_t covered = 0;          // first-byte starts already scanned
  size_t stop_at = len;        // early-stop: skip the tail entirely
  for (size_t i = 0; i <= end; i += 32) {
    const __m256i v =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
    unsigned mask =
        static_cast<unsigned>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, first)));
    while (mask) {
      const unsigned b = static_cast<unsigned>(__builtin_ctz(mask));
      mask &= mask - 1;
      const size_t pos = i + b;
      if (pos < skip_until) continue;
      if (pos + nlen <= len && memcmp(data + pos, needle, nlen) == 0) {
        ++count;
        skip_until = pos + nlen;
        if (!cb(pos)) {
          stop_at = 0; // run nothing after this
          goto done;
        }
      }
    }
    covered = i + 32;
  }
done:
  if (stop_at != 0) {
    // Scalar tail: start positions the vector loop never covered.
    // Honors skip_until from the vector phase.
    size_t p = covered;
    if (p < skip_until) p = skip_until;
    while (p < len) {
      const void* hit = memchr(data + p, needle[0], len - p);
      if (!hit) break;
      size_t off = static_cast<const char*>(hit) - data;
      if (off < skip_until) {
        p = off + 1;
        continue;
      }
      if (off + nlen > len) break;
      if (memcmp(data + off, needle, nlen) == 0) {
        ++count;
        skip_until = off + nlen;
        if (!cb(off)) break;
        p = skip_until;
      } else {
        p = off + 1;
      }
    }
  }
  return count;
}
#endif // HGREP_HAVE_X86_SIMD

// Non-overlapping match offsets, ascending. cb returns false to stop
// early. Returns hit count.
template <typename Cb>
size_t find_all_exact(const char* data, size_t len, const char* needle,
                      size_t nlen, Cb&& cb) {
  if (nlen == 0 || nlen > len) return 0;
  if (nlen == 1) {
    // Single byte: memchr loop is unbeatable.
    size_t count = 0;
    const char* p = data;
    size_t left = len;
    while (left > 0) {
      const void* hit = memchr(p, needle[0], left);
      if (!hit) break;
      size_t off = static_cast<const char*>(hit) - data;
      ++count;
      if (!cb(off)) break;
      p = static_cast<const char*>(hit) + 1;
      left = len - static_cast<size_t>(p - data);
    }
    return count;
  }
#ifdef HGREP_HAVE_X86_SIMD
  {
    static const bool has_avx2 = __builtin_cpu_supports("avx2");
    if (has_avx2 && len >= 32 && nlen >= 2)
      return find_all_avx2(data, len, needle, nlen, std::forward<Cb>(cb));
  }
#endif
  size_t count = 0, pos = 0;
  for (;;) {
    const char* hit = detail::memmem_find(data + pos, len - pos, needle, nlen);
    if (!hit) break;
    size_t off = static_cast<size_t>(hit - data);
    ++count;
    pos = off + nlen;
    if (!cb(off)) break;
    if (pos >= len) break;
  }
  return count;
}

template <typename Cb>
size_t find_all_ci(const char* data, size_t len,
                   const detail::HorspoolCI& hs, Cb&& cb) {
  size_t m = hs.needle.size();
  if (m == 0 || m > len) return 0;
  size_t count = 0, pos = 0;
  for (;;) {
    const char* hit = hs.find(data + pos, len - pos);
    if (!hit) break;
    size_t off = static_cast<size_t>(hit - data);
    ++count;
    pos = off + m;
    if (!cb(off)) break;
    if (pos >= len) break;
  }
  return count;
}

} // namespace hgrep
