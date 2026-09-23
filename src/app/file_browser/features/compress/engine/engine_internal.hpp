#pragma once
// Shared internals for the compress-engine translation units.
// Only compress/engine/*.cpp includes this; everything else goes through
// compress_engine.hpp (plan types + supports/dispatch entry points).
#include <cstdint>
#include <memory>
#include <string>

#include "app/file_browser/features/compress/compress_engine.hpp"

#if defined(EH_HAVE_ZLIB) || defined(EH_HAVE_LZMA)
// Must stay at global scope: never include system headers from inside a
// namespace (their symbols would land in ours).
#include <zlib.h>
#endif

namespace eh::file_browser {

struct OperationProgress;

// tar container helpers (engine_plan.cpp, no codec deps).
void put_octal(char* dst, size_t w, uint64_t v);
void append_tar_header(std::string& out, const std::string& name, uint64_t size,
                       unsigned mode, int64_t mtime, int uid, int gid,
                       char typeflag, const std::string& linkname);

#if defined(EH_HAVE_ZLIB) || defined(EH_HAVE_LZMA)
// Chunked CRC32 (engine_plan.cpp). Needs zlib types; available whenever any
// engine codec is enabled (all engines CRC their streams today).
uLong crc_run(uLong crc, const char* data, size_t len);
#endif

// Per-format writers (defined in their own translation units).
bool engine_compress_tar_gz(const EnginePlan& plan, const std::string& archive_path,
                            int level, int threads,
                            const std::shared_ptr<OperationProgress>& prog);
bool engine_compress_zip(const EnginePlan& plan, const std::string& archive_path,
                         int level, int threads,
                         const std::shared_ptr<OperationProgress>& prog);
bool engine_compress_tar_xz(const EnginePlan& plan, const std::string& archive_path,
                            int level, int threads,
                            const std::shared_ptr<OperationProgress>& prog);
bool engine_compress_7z(const EnginePlan& plan, const std::string& archive_path,
                        int level, int threads,
                        const std::shared_ptr<OperationProgress>& prog);

} // namespace eh::file_browser
