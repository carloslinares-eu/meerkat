/**
 * @file sha256.cpp
 * @brief Implementation of SHA-256 (FIPS 180-4, section 6.2).
 */
#include "store/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <vector>

namespace meerkat::store {

namespace {

/// Round constants: the first 32 bits of the fractional parts of the cube
/// roots of the first 64 primes.
constexpr std::array<std::uint32_t, 64> kK{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/**
 * @brief Rotates @p x right by @p n bits.
 * @param x Value.
 * @param n Bits, 1–31.
 * @return The rotated value.
 */
constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

}  // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::compress(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = static_cast<std::uint32_t>(block[4 * i]) << 24 |
               static_cast<std::uint32_t>(block[4 * i + 1]) << 16 |
               static_cast<std::uint32_t>(block[4 * i + 2]) << 8 |
               static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = h_;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(std::string_view data) {
    length_ += data.size();
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t n = data.size();
    if (buffered_ > 0) {
        const std::size_t take = std::min(n, buffer_.size() - buffered_);
        std::copy_n(p, take, buffer_.data() + buffered_);
        buffered_ += take;
        p += take;
        n -= take;
        if (buffered_ < buffer_.size()) {
            return;
        }
        compress(buffer_.data());
        buffered_ = 0;
    }
    for (; n >= 64; p += 64, n -= 64) {
        compress(p);
    }
    std::copy_n(p, n, buffer_.data());
    buffered_ = n;
}

Sha256::Digest Sha256::finish() {
    const std::uint64_t bits = length_ * 8;
    // Padding: 0x80, zeros up to 56 mod 64, then the length in bits (big endian).
    std::string pad(1, '\x80');
    const std::size_t used = (buffered_ + 1) % 64;
    pad.append(used <= 56 ? 56 - used : 120 - used, '\0');
    for (int shift = 56; shift >= 0; shift -= 8) {
        pad += static_cast<char>((bits >> shift) & 0xffu);
    }
    const std::uint64_t saved = length_;
    update(pad);
    length_ = saved;

    Digest d{};
    for (std::size_t i = 0; i < 8; ++i) {
        d[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
        d[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        d[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
        d[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
    return d;
}

std::string Sha256::hex(const Digest& d) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t byte : d) {
        out += kDigits[byte >> 4];
        out += kDigits[byte & 0xf];
    }
    return out;
}

std::string sha256_hex(std::string_view data) {
    Sha256 h;
    h.update(data);
    return Sha256::hex(h.finish());
}

std::optional<std::string> sha256_file(const std::filesystem::path& path,
                                       std::optional<std::uint64_t> length) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    Sha256 h;
    std::vector<char> buf(1u << 16);
    std::uint64_t remaining = length.value_or(UINT64_MAX);
    while (remaining > 0) {
        const auto want = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buf.size()));
        in.read(buf.data(), want);
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        h.update({buf.data(), static_cast<std::size_t>(got)});
        remaining -= static_cast<std::uint64_t>(got);
    }
    if (in.bad() || (length && remaining > 0)) {
        return std::nullopt;  // read error, or the file is shorter than asked
    }
    return Sha256::hex(h.finish());
}

}  // namespace meerkat::store
