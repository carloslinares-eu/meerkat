/**
 * @file limit.hpp
 * @brief The speed-test service's limit as `meerkat run` and `meerkat once`
 *        see it: the block in the data directory, the skip row, the update.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "core/measurement.hpp"
#include "core/rate_limit.hpp"

namespace meerkat::app {

/**
 * @brief Reads the block recorded in @p data_dir.
 * @param data_dir The data directory.
 * @param warn     Told why, if the file can't be read; the block is then
 *                 taken as none.
 * @return The block, if any.
 */
std::optional<core::RateLimitState> load_rate_limit(const std::filesystem::path& data_dir,
                                                    const std::function<void(const std::string&)>& warn);

/**
 * @brief The result of a speed test not started because of a block.
 * @param backend The speed-test backend's name.
 * @param until   The end of the wait.
 * @return Status `rate_limited`; the error says until when, in UTC.
 */
core::SpeedResult rate_limit_skip(std::string backend, core::Timestamp until);

/**
 * @brief Records the block after a test that ran, if it changed.
 * @param data_dir The data directory.
 * @param before   The block before the test.
 * @param status   The test's status.
 * @param started  When the round started.
 * @param warn     Told why, if the file can't be written.
 * @return The new block, if any.
 */
std::optional<core::RateLimitState> update_rate_limit(const std::filesystem::path& data_dir,
                                                      const std::optional<core::RateLimitState>& before,
                                                      core::Status status, core::Timestamp started,
                                                      const std::function<void(const std::string&)>& warn);

}  // namespace meerkat::app
