/**
 * @file rate_limit.cpp
 * @brief The speed-test service's limit: waits and their end.
 */
#include "core/rate_limit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace meerkat::core {

std::chrono::seconds rate_limit_wait(int refusals) {
    using std::chrono::hours;
    static constexpr std::array<hours, 4> kWaits{hours{1}, hours{2}, hours{4}, hours{8}};
    const auto i = static_cast<std::size_t>(std::clamp(refusals, 1, static_cast<int>(kWaits.size())) - 1);
    return kWaits[i];
}

std::optional<Timestamp> rate_limit_waiting(const std::optional<RateLimitState>& state, Timestamp now) {
    if (state && now < state->until) {
        return state->until;
    }
    return std::nullopt;
}

std::optional<RateLimitState> rate_limit_after(const std::optional<RateLimitState>& before, Status status,
                                               Timestamp started) {
    if (status == Status::ok) {
        return std::nullopt;
    }
    if (status != Status::rate_limited) {
        return before;
    }
    // The count stops at the longest wait, so it can't grow without end.
    const int refusals = before ? std::min(before->refusals + 1, 4) : 1;
    return RateLimitState{.until = started + rate_limit_wait(refusals), .refusals = refusals};
}

}  // namespace meerkat::core
