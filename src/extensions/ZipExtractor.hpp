// Aurora — narrow ZIP extractor used by packaged Chrome extensions.
//
// CRX payloads are ordinary ZIP archives, but the core library deliberately
// has no Qt/CEF dependency and the project does not ship a general archive
// library. This reader therefore supports only the two methods Chrome
// extension packages use (stored and raw DEFLATE), validates central-directory
// metadata, and refuses traversal, links, encryption, ZIP64 and archive bombs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aurora::extensions {

struct ZipExtractResult {
    bool ok = false;
    std::string detail;
};

// Extract the ZIP beginning at `zip_offset` in `archive_path` into the already
// empty `destination`. No path may escape destination. Partial output is left
// for the caller to remove when an error is returned.
ZipExtractResult extract_zip_payload(std::string_view archive_path,
                                     std::uint64_t zip_offset,
                                     std::string_view destination);

} // namespace aurora::extensions
