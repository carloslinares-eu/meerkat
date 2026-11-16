/**
 * @file rate_limit.hpp
 * @brief Waiting out the speed-test service's limit (Ookla: "Limit reached").
 */
#pragma once

#include <chrono>
#include <optional>

#include "core/time.hpp"
#include "core/status.hpp"

namespace meerkat::core {

/// The first wait after the service refuses a test.
inline constexpr std::chrono::hours kRateLimitFirstWait{1};
/// The longest wait: it doubles with every refusal in a row up to this.
inline constexpr std::chrono::hours kRateLimitMaxWait{8};

/**
 * @brief When the speed-test service refuses because of too many tests,
 *        speed tests wait before trying again (#125).
 *
 * Trying again at the usual interval keeps the block going: the service
 * counts the refused tries too. So the first refusal waits
 * kRateLimitFirstWait, each further one in a row twice as long, up to
 * kRateLimitMaxWait. A test that gets through (any status other than
 * `rate_limited`) ends the series.
 */
class RateLimitBackoff {
public:
    /**
     * @brief Whether speed tests wait now.
     * @param now The current time.
     * @return The end of the wait, or empty if a test may run.
     */
    [[nodiscard]] std::optional<Timestamp> waiting(Timestamp now) const;

    /**
     * @brief Takes the result of a test that ran.
     * @param status Its status.
     * @param now    When it ended.
     * @return The end of the new wait if the service refused, else empty.
     */
    std::optional<Timestamp> record(Status status, Timestamp now);

private:
    std::optional<Timestamp> until_;  ///< End of the current wait.
    int refusals_ = 0;                ///< Refusals in a row.
};

}  // namespace meerkat::core
