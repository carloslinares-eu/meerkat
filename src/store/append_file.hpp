/**
 * @file append_file.hpp
 * @brief A locked, append-only file whose writes are synced to disk.
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace meerkat::store {

/**
 * @brief The file handling shared by meerkat's data files (CSVs, raw log).
 *
 * - Opens or creates a regular file for appending (`O_APPEND`); new files
 *   get mode 0640 (before the umask) and their directory is synced, so the
 *   new entry survives a crash.
 * - Takes an exclusive `flock()`; a second writer on the same file fails
 *   instead of interleaving data.
 * - append() writes everything with `write()` and then `fdatasync()`s. If
 *   either fails, the file is closed: it may end in part of the data, and
 *   after a failed sync Linux may have dropped earlier unsynced pages and
 *   cleared the error. Reopen it to go on, and repair the tail.
 *
 * What a torn tail looks like and how to repair it is up to the format, so
 * the users of this class do that themselves (with read() and append()).
 */
class AppendFile {
public:
    /// A closed file.
    AppendFile() = default;

    /**
     * @brief Opens (or creates) @p path for appending and locks it.
     * @param path The file; its directory must exist.
     * @throws std::system_error if it can't be opened, isn't a regular file,
     *                           or another process holds the lock (EWOULDBLOCK).
     */
    explicit AppendFile(std::filesystem::path path);

    /// Closes the file, which releases the lock.
    ~AppendFile();

    AppendFile(const AppendFile&) = delete;             ///< Owns a descriptor: not copyable.
    AppendFile& operator=(const AppendFile&) = delete;  ///< Owns a descriptor: not copyable.

    /**
     * @brief Takes over @p other's file; @p other is left closed.
     * @param other The file to move from.
     */
    AppendFile(AppendFile&& other) noexcept;

    /**
     * @brief Closes this file and takes over @p other's.
     * @param other The file to move from.
     * @return This object.
     */
    AppendFile& operator=(AppendFile&& other) noexcept;

    /// @return True while the file is open.
    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    /// @return True if the constructor created the file (it didn't exist).
    [[nodiscard]] bool created() const noexcept { return created_; }

    /// @return The file's path.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /**
     * @brief The file's current size.
     * @return Size in bytes.
     * @throws std::logic_error  if the file is closed.
     * @throws std::system_error if `fstat()` fails.
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief Reads up to @p count bytes starting at @p offset.
     * @param offset Where to start.
     * @param count  Maximum number of bytes.
     * @return The bytes; fewer than @p count at the end of the file.
     * @throws std::logic_error  if the file is closed.
     * @throws std::system_error if reading fails.
     */
    [[nodiscard]] std::string read(std::size_t offset, std::size_t count) const;

    /**
     * @brief Appends @p data and syncs it to disk.
     * @param data The bytes.
     * @throws std::logic_error  if the file is closed.
     * @throws std::system_error if writing or syncing fails; the file is then
     *                           closed.
     */
    void append(std::string_view data);

    /**
     * @brief Replaces the whole content with @p data and syncs it.
     *
     * Only for repairing a file that holds nothing but an interrupted header.
     *
     * @param data The new content.
     * @throws std::logic_error  if the file is closed.
     * @throws std::system_error if it fails; the file is then closed.
     */
    void rewrite(std::string_view data);

    /// Closes the file if it is open.
    void close() noexcept;

private:
    /**
     * @brief Throws std::logic_error if the file is closed.
     * @param what The operation, for the message.
     */
    void require_open(std::string_view what) const;

    std::filesystem::path path_;  ///< The file.
    int fd_ = -1;                 ///< Descriptor; -1 when closed.
    bool created_ = false;        ///< The constructor created the file.
};

/**
 * @brief fsync()s the directory containing @p path, so a new or renamed entry
 *        is durable.
 * @param path A file in the directory.
 * @throws std::system_error if the directory can't be opened or synced.
 */
void sync_parent_dir(const std::filesystem::path& path);

}  // namespace meerkat::store
