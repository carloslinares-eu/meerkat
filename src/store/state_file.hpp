/**
 * @file state_file.hpp
 * @brief Small one-line state files in the data directory (pause, rate limit).
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace meerkat::store {

/**
 * @brief Reads a small one-line state file (pause, rate limit).
 *
 * Never blocks: a FIFO in its place is refused (#75).
 * @param path The file.
 * @return Its content without one trailing newline; nothing if it doesn't exist.
 * @throws std::system_error, sys::NotRegularFile as sys::read_regular_file().
 */
std::optional<std::string> read_state_file(const std::filesystem::path& path);

/**
 * @brief Removes a state file and syncs its directory.
 * @param path The file.
 * @return True if it was there.
 * @throws std::system_error if it can't be removed.
 */
bool remove_state_file(const std::filesystem::path& path);

}  // namespace meerkat::store
