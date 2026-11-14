/**
 * @file regular_file.hpp
 * @brief Reads the start of a file that must be a regular file: never
 *        follows a symlink, never waits on a FIFO.
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "sys/fd.hpp"

namespace meerkat::sys {

/// The path exists but isn't a regular file (a FIFO, a device, a directory).
class NotRegularFile : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;  ///< Takes the message.
};

/**
 * @brief Opens @p path for reading, only if it is a regular file.
 *
 * Opened with `O_NOFOLLOW | O_NONBLOCK | O_NOCTTY`, then checked with
 * `fstat()`, so a symlink is refused, a FIFO swapped in never blocks and a
 * terminal never becomes the controlling one.
 * @param path The file.
 * @return The open descriptor.
 * @throws std::system_error with the errno of `open()` or `fstat()`:
 *         `ENOENT` if it doesn't exist, `ELOOP` if it is a symlink, `EACCES`
 *         if this user can't read it.
 * @throws NotRegularFile if it isn't a regular file.
 */
Fd open_regular_file(const std::filesystem::path& path);

/**
 * @brief Reads up to @p max bytes from the start of @p path, opened with
 *        open_regular_file().
 *
 * Short reads and EINTR are retried.
 * @param path The file.
 * @param max  Most bytes to read.
 * @return The bytes; fewer at the end of the file.
 * @throws std::system_error with the errno of `open()`, `fstat()` or
 *         `read()`: `ENOENT` if it doesn't exist, `ELOOP` if it is a
 *         symlink, `EACCES` if this user can't read it.
 * @throws NotRegularFile if it isn't a regular file.
 */
std::string read_regular_file(const std::filesystem::path& path, std::size_t max);

}  // namespace meerkat::sys
