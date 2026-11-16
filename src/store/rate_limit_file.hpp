/**
 * @file rate_limit_file.hpp
 * @brief The state file of the speed-test service's limit, so a restart or
 *        `meerkat once` doesn't forget a block (#126).
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "core/rate_limit.hpp"

namespace meerkat::store {

/// The file in the data directory. One line: the end of the wait in UTC and
/// the refusals in a row (`2026-03-10T13:00:00Z 2`). Like the pause file it
/// isn't evidence, so no manifest lists it.
inline constexpr std::string_view kRateLimitFile = "rate-limit";

/**
 * @brief The block recorded in @p data_dir.
 * @param data_dir The data directory.
 * @return The block, past or not; nothing if there is no file.
 * @throws std::system_error  if it is there but can't be read.
 * @throws sys::NotRegularFile if it isn't a regular file.
 * @throws std::runtime_error if it doesn't hold a time and a count.
 */
std::optional<core::RateLimitState> read_rate_limit(const std::filesystem::path& data_dir);

/**
 * @brief Records @p state in @p data_dir, or removes the file if it is empty.
 * @param data_dir The data directory; it must exist.
 * @param state    The block, or nothing.
 * @throws std::system_error if the file can't be written or removed.
 */
void write_rate_limit(const std::filesystem::path& data_dir, const std::optional<core::RateLimitState>& state);

}  // namespace meerkat::store
