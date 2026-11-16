/**
 * @file zip_reader.hpp
 * @brief Test helper: reads back a ZIP written by store::ZipWriter (no
 *        ZIP64, deflate or stored) and checks its CRCs and sizes.
 */
#pragma once

#include <zlib.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace meerkat::test {

/// One entry read back.
struct ZipEntry {
    std::string name;            ///< Path inside the archive.
    std::string data;            ///< Inflated content.
    std::uint16_t flags = 0;     ///< General purpose flags (central directory).
    std::uint16_t method = 0;    ///< Compression method.
    std::uint32_t external = 0;  ///< External attributes.
};

/// @return The little-endian 16-bit value at @p at.
inline std::uint32_t le16(std::string_view z, std::size_t at) {
    if (at + 2 > z.size()) {
        throw std::runtime_error("zip: truncated");
    }
    return static_cast<std::uint32_t>(static_cast<unsigned char>(z[at])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(z[at + 1])) << 8;
}

/// @return The little-endian 32-bit value at @p at.
inline std::uint32_t le32(std::string_view z, std::size_t at) { return le16(z, at) | le16(z, at + 2) << 16; }

/**
 * @brief Inflates raw deflate data.
 * @param in The compressed bytes.
 * @return The content.
 */
inline std::string inflate_raw(std::string_view in) {
    z_stream s{};
    if (inflateInit2(&s, -15) != Z_OK) {
        throw std::runtime_error("zip: inflateInit2");
    }
    std::string out;
    std::vector<char> buf(1 << 16);
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    s.avail_in = static_cast<uInt>(in.size());
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        s.next_out = reinterpret_cast<Bytef*>(buf.data());
        s.avail_out = static_cast<uInt>(buf.size());
        rc = inflate(&s, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&s);
            throw std::runtime_error("zip: bad deflate data");
        }
        out.append(buf.data(), buf.size() - s.avail_out);
    }
    inflateEnd(&s);
    return out;
}

/**
 * @brief Reads every entry of @p zip, checking the end record, the central
 *        directory, the local headers, the CRCs and the sizes.
 * @param zip The archive.
 * @return The entries, in order.
 * @throws std::runtime_error if anything doesn't match.
 */
inline std::vector<ZipEntry> read_zip(std::string_view zip) {
    if (zip.size() < 22 || le32(zip, zip.size() - 22) != 0x06054b50) {
        throw std::runtime_error("zip: no end record");
    }
    const auto eocd = zip.size() - 22;
    const auto count = le16(zip, eocd + 10);
    const auto dir_size = le32(zip, eocd + 12);
    auto at = static_cast<std::size_t>(le32(zip, eocd + 16));
    if (at + dir_size != eocd) {
        throw std::runtime_error("zip: central directory not where the end record says");
    }
    std::vector<ZipEntry> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (le32(zip, at) != 0x02014b50) {
            throw std::runtime_error("zip: bad central directory entry");
        }
        ZipEntry e;
        e.flags = static_cast<std::uint16_t>(le16(zip, at + 8));
        e.method = static_cast<std::uint16_t>(le16(zip, at + 10));
        const auto crc = le32(zip, at + 16);
        const auto csize = le32(zip, at + 20);
        const auto size = le32(zip, at + 24);
        const auto name_len = le16(zip, at + 28);
        const auto extra_len = le16(zip, at + 30);
        const auto comment_len = le16(zip, at + 32);
        e.external = le32(zip, at + 38);
        const auto local = static_cast<std::size_t>(le32(zip, at + 42));
        e.name = std::string{zip.substr(at + 46, name_len)};
        at += 46 + name_len + extra_len + comment_len;

        if (le32(zip, local) != 0x04034b50 || le16(zip, local + 26) != name_len ||
            zip.substr(local + 30, name_len) != e.name) {
            throw std::runtime_error("zip: local header doesn't match for " + e.name);
        }
        const auto data_at = local + 30 + name_len + le16(zip, local + 28);
        const auto raw = zip.substr(data_at, csize);
        e.data = e.method == 8 ? inflate_raw(raw) : std::string{raw};
        if (e.data.size() != size ||
            crc32(0L, reinterpret_cast<const Bytef*>(e.data.data()), static_cast<uInt>(e.data.size())) != crc) {
            throw std::runtime_error("zip: size or CRC wrong for " + e.name);
        }
        if ((e.flags & 8u) != 0) {
            const auto d = data_at + csize;
            if (le32(zip, d) != 0x08074b50 || le32(zip, d + 4) != crc || le32(zip, d + 8) != csize ||
                le32(zip, d + 12) != size) {
                throw std::runtime_error("zip: data descriptor wrong for " + e.name);
            }
        }
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace meerkat::test
