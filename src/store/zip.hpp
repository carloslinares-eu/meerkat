/**
 * @file zip.hpp
 * @brief A small ZIP writer for the export bundle: deflate (zlib), streamed
 *        file by file, readable by any unzip tool.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/time.hpp"

namespace meerkat::store {

/**
 * @brief Writes a ZIP archive (PKWARE APPNOTE, version 2.0) entry by entry.
 *
 * Each entry is deflated (raw deflate, zlib) as it is read, followed by a
 * data descriptor with its CRC-32 and sizes, so a file is never held in
 * memory whole. Names are UTF-8 (flag bit 11). No ZIP64: an entry or the
 * archive over 4 GiB, or more than 65535 entries, is an error, far beyond
 * a year of meerkat data.
 */
class ZipWriter {
public:
    /// Where the archive's bytes go, in order.
    using Sink = std::function<void(std::string_view)>;

    /**
     * @brief Starts an archive.
     * @param sink Receives the bytes.
     * @param time The modification time every entry gets, as wall-clock
     *             time: ZIP stores it without a zone, and unzip tools show
     *             it as local time, so pass local time (UTC + offset).
     */
    ZipWriter(Sink sink, core::Timestamp time);

    /**
     * @brief Adds an entry from memory.
     * @param name Path inside the archive, `/`-separated, relative.
     * @param data The content.
     * @return The SHA-256 (hex) of the content.
     * @throws std::invalid_argument for a bad name; std::length_error past
     *         the ZIP limits; std::runtime_error if zlib fails.
     */
    std::string add(std::string_view name, std::string_view data);

    /**
     * @brief Adds an entry from the first @p size bytes of a file.
     * @param name Path inside the archive.
     * @param path The file (opened with sys::open_regular_file(): no
     *             symlinks, no FIFOs).
     * @param size How many bytes to take; the file may be longer (it may
     *             still grow).
     * @return The SHA-256 (hex) of the bytes archived, taken in the same
     *         read, so it describes exactly what is in the archive.
     * @throws std::system_error if the file can't be opened or read, or
     *         std::runtime_error if it is shorter than @p size; otherwise
     *         as add().
     */
    std::string add_file(std::string_view name, const std::filesystem::path& path, std::uint64_t size);

    /**
     * @brief Writes the central directory. Nothing may be added afterwards.
     * @throws std::length_error past the ZIP limits.
     */
    void finish();

private:
    /// What the central directory needs about one entry.
    struct Entry {
        std::string name;               ///< Path inside the archive.
        std::uint32_t crc = 0;          ///< CRC-32 of the content.
        std::uint64_t compressed = 0;   ///< Deflated size.
        std::uint64_t size = 0;         ///< Original size.
        std::uint64_t offset = 0;       ///< Of its local header.
    };

    /**
     * @brief Adds one entry whose content @p read supplies in pieces.
     * @param name Path inside the archive.
     * @param read Fills its buffer and returns the bytes it put there; 0 at
     *             the end.
     * @return The SHA-256 (hex) of the content.
     */
    std::string add_stream(std::string_view name, const std::function<std::size_t(std::vector<char>&)>& read);

    /**
     * @brief Sends bytes to the sink and counts them.
     * @param bytes The bytes.
     */
    void emit(std::string_view bytes);

    Sink sink_;                     ///< The output.
    std::uint16_t dos_time_ = 0;    ///< MS-DOS time of every entry.
    std::uint16_t dos_date_ = 0;    ///< MS-DOS date of every entry.
    std::uint64_t offset_ = 0;      ///< Bytes written so far.
    std::vector<Entry> entries_;    ///< Added so far.
    bool finished_ = false;         ///< finish() was called.
};

}  // namespace meerkat::store
