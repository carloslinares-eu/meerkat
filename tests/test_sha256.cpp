/**
 * @file test_sha256.cpp
 * @brief Tests for store::Sha256 against the FIPS 180-4 / NIST vectors.
 */
#include <doctest/doctest.h>

#include <sys/stat.h>

#include <fstream>
#include <string>

#include "store/sha256.hpp"
#include "support.hpp"

using namespace meerkat::store;

TEST_CASE("SHA-256 test vectors") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
                     "klmnopqrlmnopqrsmnopqrstnopqrstu") ==
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    CHECK(sha256_hex(std::string(1'000'000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 padding boundaries") {
    // Lengths around the 55/56/64-byte padding edges, checked against sha256sum.
    CHECK(sha256_hex(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(sha256_hex(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(sha256_hex(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("SHA-256 gives the same digest however the input is split") {
    std::string data;
    for (int i = 0; i < 300; ++i) {
        data += static_cast<char>(i * 7 + 3);
    }
    const auto whole = sha256_hex(data);
    for (const std::size_t step : {1u, 3u, 63u, 64u, 65u, 127u}) {
        CAPTURE(step);
        Sha256 h;
        for (std::size_t i = 0; i < data.size(); i += step) {
            h.update(std::string_view{data}.substr(i, step));
        }
        CHECK(Sha256::hex(h.finish()) == whole);
    }
}

TEST_CASE("sha256_file hashes a file or a prefix of it") {
    meerkat::test::TempDir dir;
    const auto file = dir.path / "f";
    {
        std::ofstream out{file, std::ios::binary};
        out << "abcdef";
    }
    CHECK(sha256_file(file) == sha256_hex("abcdef"));
    CHECK(sha256_file(file, 3) == sha256_hex("abc"));
    CHECK(sha256_file(file, 0) == sha256_hex(""));
    CHECK(sha256_file(file, 6) == sha256_hex("abcdef"));
    CHECK_FALSE(sha256_file(file, 7).has_value());  // shorter than asked
    CHECK_FALSE(sha256_file(dir.path / "missing").has_value());
    // Only regular files (#75): not a symlink's target, and a FIFO without a
    // writer is refused at once instead of blocking.
    std::filesystem::create_symlink(file, dir.path / "link");
    CHECK_FALSE(sha256_file(dir.path / "link").has_value());
    REQUIRE(::mkfifo((dir.path / "fifo").c_str(), 0600) == 0);
    CHECK_FALSE(sha256_file(dir.path / "fifo").has_value());
}

TEST_CASE("sha256_prefixes hashes several prefixes in one pass") {
    meerkat::test::TempDir dir;
    const auto file = dir.path / "f";
    {
        std::ofstream out{file, std::ios::binary};
        out << std::string(70000, 'a') << "tail";
    }
    const auto h = sha256_prefixes(file, {70004, 3, 0, 3, 70000, 90000});
    CHECK(h.size() == 4);  // 90000 is past the end; 3 only once
    CHECK(h.at(0) == sha256_hex(""));
    CHECK(h.at(3) == sha256_hex("aaa"));
    CHECK(h.at(70000) == sha256_hex(std::string(70000, 'a')));
    CHECK(h.at(70004) == sha256_hex(std::string(70000, 'a') + "tail"));
    CHECK(sha256_prefixes(dir.path / "nope", {1}).empty());
    CHECK(sha256_prefixes(file, {}).empty());
}
