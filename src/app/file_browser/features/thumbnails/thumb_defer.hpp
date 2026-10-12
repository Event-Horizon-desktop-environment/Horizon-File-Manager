// thumb_defer.hpp — keep background thumbnail workers off files that are
// still being written.
//
// Demuxing/decoding a growing multi-GB download (e.g. a browser fetching a
// video straight to its final name) can wedge the single thumbnail worker
// for minutes while the on-disk bytes shift underneath it, and the result is
// uncacheable anyway because the mtime keeps moving. Deferring these paths
// (plain icon until the file settles) keeps folder opens fast; the next
// listing reload after CLOSE_WRITE picks the thumbnail up normally.
#pragma once

#include <string>

namespace eh::file_browser {

// True when path looks like an in-progress download or a large file that was
// modified seconds ago. Suffix matches apply without stating the file, so
// briefly-absent rename targets (browser .part -> final swaps) stay deferred
// too. Cheap: one stat() at most.
bool thumb_should_defer(const std::string& path);

}  // namespace eh::file_browser
