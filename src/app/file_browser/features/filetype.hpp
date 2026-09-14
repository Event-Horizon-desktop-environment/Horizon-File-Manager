#pragma once

#include "../app.hpp"

#include <sys/types.h>

namespace eh::file_browser {

// ── filetype.cpp shared surface ──────────────────────────────────
// File-type classification helpers defined in features/filetype.cpp.
// detect_file_type / mime_by_ext / user_name / group_name are consumed by
// the scan pipeline (features/scan.cpp) and the hover/space preview
// (features/preview_popup.cpp). mime_to_file_type stays filetype-internal.

const std::string& user_name(uid_t uid);
const std::string& group_name(gid_t gid);
FileType detect_file_type(const std::string& name, bool is_dir,
                          const std::string& mime_type,
                          const std::string& full_path = {},
                          const std::string& ext_hint = {});
std::string mime_by_ext(const std::string& path);

} // namespace eh::file_browser