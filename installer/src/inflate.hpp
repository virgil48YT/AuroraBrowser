// Aurora Browser Setup — streaming raw DEFLATE decoder.
//
// The installer cannot link zlib/Qt, and buffering a whole libcef.dll in memory
// just to unpack it is wasteful. This decoder reads a complete compressed
// entry (the largest CEF file is well under the cap enforced by payload.cpp)
// but writes decompressed bytes straight into a sink, so memory stays bounded
// while the progress bar keeps moving.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace aurora::setup {

class InflateSink {
public:
    virtual ~InflateSink() = default;
    // Return false on failure and set `error`/`error_detail`.
    virtual bool write(const std::uint8_t* data, std::size_t size) = 0;

    HRESULT error = S_OK;
    std::wstring error_detail;
};

struct InflateResult {
    bool ok = false;
    HRESULT error = S_OK;
    std::wstring detail;
    std::uint64_t produced = 0;
};

InflateResult inflate_raw_stream(const std::uint8_t* data, std::size_t size,
                                 std::uint64_t expected, InflateSink& sink);

} // namespace aurora::setup
