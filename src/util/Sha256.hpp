// Aurora — SHA-256.
//
// A small, dependency-free SHA-256 implementation. Aurora needs a stable
// cryptographic digest in the *core* library (no Qt, no CEF, no OpenSSL) for
// two reasons:
//
//   1. The build ID (see core/BuildId.hpp) is a hash over the CEF runtime
//      files, and it has to be computed identically by the packaging tool that
//      writes the manifest and by the bootstrap launcher that verifies it at
//      runtime. Sharing one implementation is what makes those two agree.
//   2. Per-file integrity checks of an extracted CEF tree.
//
// This is FIPS 180-4 SHA-256, streaming, no allocation beyond the object
// itself. It is not constant-time and is not intended for secret material —
// only for content addressing and integrity.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace aurora::util {

class Sha256 {
public:
    static constexpr size_t kDigestSize = 32;
    using Digest = std::array<uint8_t, kDigestSize>;

    Sha256() { reset(); }

    void reset();
    void update(const void* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }

    // Finalizes the hash. The object must not be updated afterwards without
    // reset(); finish() is idempotent and always returns the same digest.
    Digest finish();

    // Lowercase hex of finish().
    std::string finish_hex();

private:
    void compress(const uint8_t block[64]);

    std::array<uint32_t, 8> h_{};
    std::array<uint8_t, 64> buf_{};
    size_t buf_len_ = 0;
    uint64_t total_bits_ = 0;
    bool finished_ = false;
    Digest digest_{};
};

// Convenience: lowercase hex digest of a buffer / string.
std::string sha256_hex(std::string_view data);

// Lowercase hex digest of a file's contents, streamed in fixed-size chunks so
// a 200 MB libcef.dll never lands in memory. Returns an empty string if the
// file cannot be read.
std::string sha256_file_hex(const std::string& path);

} // namespace aurora::util
