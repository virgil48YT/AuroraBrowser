// Aurora — QR code encoding for "Share as QR code".
//
// A thin, exception-free wrapper over the vendored Project Nayuki encoder
// (third_party/qrcodegen, MIT): byte mode, error correction level Medium
// (boosted to the highest level that fits the same version), automatic
// version and mask selection. The result is a plain module matrix so the Qt
// side can paint it at any scale and the core stays Qt-free and testable.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace aurora::util {

struct QrMatrix {
    int size = 0;                // modules per side (21..177)
    std::vector<bool> modules;   // row-major, true = dark

    bool dark(int x, int y) const {
        return x >= 0 && y >= 0 && x < size && y < size &&
               modules[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) +
                       static_cast<std::size_t>(x)];
    }
};

// Encode UTF-8 `text`. nullopt when it does not fit a version-40 symbol
// (about 2.3 KB at level Medium) — a URL that long is not worth scanning.
std::optional<QrMatrix> encode_qr(std::string_view text);

} // namespace aurora::util
