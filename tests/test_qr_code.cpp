// QR encoding for "Share as QR code". The structural checks (size per
// version, finder patterns, timing pattern) are what a scanner relies on.
#include <doctest.h>

#include <string>

#include "util/QrCode.hpp"

using aurora::util::encode_qr;
using aurora::util::QrMatrix;

namespace {
// The 7x7 finder pattern with its top-left corner at (ox, oy).
bool has_finder(const QrMatrix& m, int ox, int oy) {
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 7; ++x) {
            const bool ring = x == 0 || y == 0 || x == 6 || y == 6;
            const bool core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
            if (m.dark(ox + x, oy + y) != (ring || core)) return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("qr: a short URL fits a small symbol") {
    const auto m = encode_qr("https://example.com/");
    REQUIRE(m.has_value());
    CHECK(m->size == 25);  // version 2
    CHECK(m->modules.size() == 625u);
    CHECK(has_finder(*m, 0, 0));
    CHECK(has_finder(*m, m->size - 7, 0));
    CHECK(has_finder(*m, 0, m->size - 7));
    // Timing pattern: row 6 alternates between the finders.
    for (int x = 8; x < m->size - 8; ++x) CHECK(m->dark(x, 6) == (x % 2 == 0));
}

TEST_CASE("qr: size grows with the payload and stays square") {
    const auto small = encode_qr("a");
    const auto big = encode_qr(std::string(500, 'x'));
    REQUIRE(small.has_value());
    REQUIRE(big.has_value());
    CHECK(small->size == 21);  // version 1
    CHECK(big->size > small->size);
    CHECK((big->size - 17) % 4 == 0);  // size = 17 + 4 * version
}

TEST_CASE("qr: out of range input") {
    CHECK_FALSE(encode_qr(std::string(5000, 'x')).has_value());
    const auto empty = encode_qr("");
    REQUIRE(empty.has_value());
    CHECK(empty->size == 21);
    CHECK_FALSE(empty->dark(-1, 0));
    CHECK_FALSE(empty->dark(0, 21));
}

TEST_CASE("qr: UTF-8 text is encoded as bytes") {
    const auto m = encode_qr("https://例え.jp/パス");
    REQUIRE(m.has_value());
    CHECK(m->size >= 25);
}
