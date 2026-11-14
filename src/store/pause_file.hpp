/**
 * @file pause_file.hpp
 * @brief The state file of `meerkat pause`: until when speed tests wait for
 *        an official measurement.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "core/time.hpp"

namespace meerkat::store {

/// The pause file in the data directory. It holds one line, the end of the
/// pause in UTC (`2026-03-10T14:00:00Z`). The running meerkat reads it every
/// round (core::OfficialGuard); it isn't part of the evidence, so no
/// manifest lists it.
inline constexpr std::string_view kPauseFile = "pause";

/**
 * @brief The end of the pause set in @p data_dir.
 * @param data_dir The data directory.
 * @return The time in the pause file, past or not; nothing if there is no
 *         pause file.
 * @throws std::system_error  if it is there but can't be read (a symlink
 *                            is refused: ELOOP).
 * @throws std::runtime_error if it doesn't hold a time.
 */
std::optional<core::Timestamp> read_pause(const std::filesystem::path& data_dir);

/**
 * @brief Sets the pause in @p data_dir to end at @p until, replacing one
 *        that is set.
 *
 * Written to a temp file first and renamed over the old one, so a reader
 * sees the old pause or the new one, never half a line. Mode 0644: the
 * running meerkat may be another user (the service), and the file holds
 * nothing private.
 * @param data_dir The data directory; it must exist.
 * @param until    End of the pause.
 * @throws std::system_error if the file can't be written.
 */
void write_pause(const std::filesystem::path& data_dir, core::Timestamp until);

/**
 * @brief Ends the pause: removes the pause file.
 * @param data_dir The data directory.
 * @return True if there was one.
 * @throws std::system_error if it is there but can't be removed.
 */
bool remove_pause(const std::filesystem::path& data_dir);

}  // namespace meerkat::store
