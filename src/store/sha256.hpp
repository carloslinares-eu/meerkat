/**
 * @file sha256.hpp
 * @brief SHA-256 (FIPS 180-4), small and dependency-free.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace meerkat::store {

/**
 * @brief Incremental SHA-256.
 *
 * meerkat carries its own implementation so the binary needs no OpenSSL; it
 * is checked against the FIPS 180-4 / NIST test vectors. The result is the
 * same as `sha256sum`'s.
 */
class Sha256 {
public:
    /// The 32-byte digest.
    using Digest = std::array<std::uint8_t, 32>;

    /// Starts a new hash.
    Sha256();

    /**
     * @brief Adds @p data to the hash.
     * @param data Bytes.
     */
    void update(std::string_view data);

    /**
     * @brief Finishes the hash. The object must not be updated afterwards.
     * @return The digest.
     */
    Digest finish();

    /**
     * @brief Lowercase hex of a digest, as `sha256sum` prints it.
     * @param d The digest.
     * @return 64 hex digits.
     */
    static std::string hex(const Digest& d);

private:
    /**
     * @brief Processes one 64-byte block.
     * @param block The block.
     */
    void compress(const std::uint8_t* block);

    std::array<std::uint32_t, 8> h_{};       ///< Hash state.
    std::array<std::uint8_t, 64> buffer_{};  ///< Bytes not yet processed.
    std::size_t buffered_ = 0;               ///< Bytes in buffer_.
    std::uint64_t length_ = 0;               ///< Total bytes added.
};

/**
 * @brief SHA-256 of @p data, as lowercase hex.
 * @param data Bytes.
 * @return 64 hex digits.
 */
std::string sha256_hex(std::string_view data);

/**
 * @brief SHA-256 of the first @p length bytes of a file (all of it if empty).
 * @param path   The file.
 * @param length How many bytes to hash; empty: the whole file.
 * @return 64 hex digits, or `std::nullopt` if the file can't be read, isn't
 *         a regular file (a symlink included: it is opened with
 *         sys::open_regular_file()), or is shorter than @p length.
 */
std::optional<std::string> sha256_file(const std::filesystem::path& path,
                                       std::optional<std::uint64_t> length = std::nullopt);

/**
 * @brief SHA-256 of several prefixes of one file, read once.
 *
 * For an append-only file sealed at many sizes this costs one pass instead
 * of one pass per size.
 * @param path  The file (opened with sys::open_regular_file()).
 * @param sizes The prefix lengths, in any order; duplicates are fine.
 * @return Hex digest per length; a length past the end of the file has no
 *         entry. Empty if the file can't be opened or read.
 */
std::map<std::uint64_t, std::string> sha256_prefixes(const std::filesystem::path& path,
                                                     std::vector<std::uint64_t> sizes);

}  // namespace meerkat::store
