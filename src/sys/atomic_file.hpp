/**
 * @file atomic_file.hpp
 * @brief An output file that appears complete or not at all: written to a
 *        temporary name next to it, then renamed into place.
 */
#pragma once

#include <filesystem>
#include <string_view>

namespace meerkat::sys {

/**
 * @brief Writes a new file through a temporary one and rename().
 *
 * The temporary file is created next to the target (same filesystem) with
 * mode 0600, `O_EXCL` and `O_NOFOLLOW`. commit() syncs it, renames it over
 * the target and syncs the directory; without commit() (an exception, say)
 * the destructor removes it, so a half-written file never replaces a good
 * one.
 */
class AtomicFile {
public:
    /**
     * @brief Creates the temporary file for @p path.
     * @param path The file to write in the end.
     * @throws std::system_error if it can't be created.
     */
    explicit AtomicFile(std::filesystem::path path);

    /// Removes the temporary file unless commit() succeeded.
    ~AtomicFile();

    AtomicFile(const AtomicFile&) = delete;             ///< Owns a file: not copyable.
    AtomicFile& operator=(const AtomicFile&) = delete;  ///< Owns a file: not copyable.

    /**
     * @brief Appends @p data.
     * @param data Bytes.
     * @throws std::system_error if writing fails.
     */
    void write(std::string_view data);

    /**
     * @brief Syncs the file and renames it into place.
     * @throws std::system_error if syncing, closing or renaming fails.
     */
    void commit();

private:
    std::filesystem::path path_;  ///< The target.
    std::filesystem::path tmp_;   ///< The temporary file.
    int fd_ = -1;                 ///< Open on tmp_ until commit().
    bool done_ = false;           ///< Whether commit() succeeded.
};

}  // namespace meerkat::sys
