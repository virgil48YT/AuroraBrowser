// SHA-256 is the foundation of the build ID: if it is wrong, or if it is right
// but not byte-identical between the packaging tool and the launcher, every
// per-version extraction is either rejected forever or accepted when it should
// not be. These are the FIPS 180-4 / NIST published vectors plus the block
// boundaries where a hand-written implementation actually breaks (55, 56, 63,
// 64, 65 bytes — around the padding and the length field).
#include <doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "util/Sha256.hpp"

using aurora::util::Sha256;
using aurora::util::sha256_file_hex;
using aurora::util::sha256_hex;

TEST_CASE("published SHA-256 vectors") {
    CHECK(sha256_hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("padding boundaries") {
    // 55 bytes: the last block still fits the length field.
    CHECK(sha256_hex(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    // 56 bytes: the length field spills into an extra block.
    CHECK(sha256_hex(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(sha256_hex(std::string(63, 'a')) ==
          "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
    CHECK(sha256_hex(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    CHECK(sha256_hex(std::string(65, 'a')) ==
          "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
    CHECK(sha256_hex(std::string(1000, 'a')) ==
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("streaming in arbitrary chunks matches a single update") {
    const std::string data(5000, 'x');
    Sha256 one;
    one.update(data);
    Sha256 many;
    size_t pos = 0;
    for (size_t chunk : {1u, 2u, 63u, 64u, 65u, 127u, 500u, 4096u}) {
        const size_t n = std::min(chunk, data.size() - pos);
        many.update(data.data() + pos, n);
        pos += n;
        if (pos == data.size()) break;
    }
    many.update(data.data() + pos, data.size() - pos);
    CHECK(one.finish_hex() == many.finish_hex());
}

TEST_CASE("finish is idempotent and reset restarts the hash") {
    Sha256 h;
    h.update("abc");
    const std::string first = h.finish_hex();
    CHECK(h.finish_hex() == first);
    h.reset();
    h.update("abc");
    CHECK(h.finish_hex() == first);
}

TEST_CASE("file hashing matches buffer hashing") {
    namespace fs = std::filesystem;
    const fs::path p = fs::temp_directory_path() / "aurora_sha256_test.bin";
    const std::string data(300000, 'q');
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << data;
    }
    CHECK(sha256_file_hex(p.string()) == sha256_hex(data));
    fs::remove(p);
    // A missing file is an empty digest, never a plausible-looking hash.
    CHECK(sha256_file_hex(p.string()).empty());
}
