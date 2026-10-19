// Aurora — QR code encoding. See QrCode.hpp.
#include "util/QrCode.hpp"

#include <exception>
#include <string>

#include "qrcodegen.hpp"

namespace aurora::util {

std::optional<QrMatrix> encode_qr(std::string_view text) {
    try {
        const std::string owned(text);  // the encoder wants a NUL-terminated string
        const auto qr = qrcodegen::QrCode::encodeText(owned.c_str(),
                                                      qrcodegen::QrCode::Ecc::MEDIUM);
        QrMatrix m;
        m.size = qr.getSize();
        m.modules.resize(static_cast<std::size_t>(m.size) * static_cast<std::size_t>(m.size));
        for (int y = 0; y < m.size; ++y)
            for (int x = 0; x < m.size; ++x)
                m.modules[static_cast<std::size_t>(y) * static_cast<std::size_t>(m.size) +
                          static_cast<std::size_t>(x)] = qr.getModule(x, y);
        return m;
    } catch (const std::exception&) {
        // qrcodegen::data_too_long (a length_error) is the only expected one.
        return std::nullopt;
    }
}

} // namespace aurora::util
