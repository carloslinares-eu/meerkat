/**
 * @file rate_limit.hpp
 * @brief Waiting out the speed-test service's limit (Ookla: "Limit reached").
 */
#pragma once

#include <chrono>
#include <optional>

#include "core/status.hpp"
#include "core/time.hpp"

namespace meerkat::core {

/**
 * @brief A block by the speed-test service: until when speed tests wait,
 *        and how many refusals in a row led to it (#125).
 */
struct RateLimitState {
    Timestamp until;   ///< End of the wait.
    int refusals = 1;  ///< Refusals in a row (1 or more).

    /// Field-wise equality.
    bool operator==(const RateLimitState&) const = default;
};

/**
 * @brief How long speed tests wait after @p refusals refusals in a row.
 *
 * Trying again at the usual interval keeps the block going: the service
 * counts refused tries too. So it is 1 h after the first refusal, then 2 h,
 * 4 h, and 8 h from the fourth on.
 * @param refusals Refusals in a row (1 or more).
 * @return The wait.
 */
std::chrono::seconds rate_limit_wait(int refusals);

/**
 * @brief Whether speed tests wait at @p now.
 * @param state The current block, if any.
 * @param now   The start of the round.
 * @return The end of the wait, or empty if a test may run.
 */
std::optional<Timestamp> rate_limit_waiting(const std::optional<RateLimitState>& state, Timestamp now);

/**
 * @brief The block after a speed test that ran.
 *
 * A refusal (`rate_limited`) starts or extends the block, timed from the
 * round's start @p started, so the round one wait later may test. A test
 * with a result (`ok`) ends it. Anything else (the line was down, the test
 * failed) says nothing about the service's limit and keeps the count.
 * @param before  The block before the test.
 * @param status  The test's status.
 * @param started When the round started.
 * @return The new block, or empty if there is none.
 */
std::optional<RateLimitState> rate_limit_after(const std::optional<RateLimitState>& before, Status status,
                                               Timestamp started);

}  // namespace meerkat::core
