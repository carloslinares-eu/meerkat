/**
 * @file rate_limit.cpp
 * @brief RateLimitBackoff.
 */
#include "core/rate_limit.hpp"

#include <algorithm>

namespace meerkat::core {

std::optional<Timestamp> RateLimitBackoff::waiting(Timestamp now) const {
    if (until_ && now < *until_) {
        return until_;
    }
    return std::nullopt;
}

std::optional<Timestamp> RateLimitBackoff::record(Status status, Timestamp now) {
    if (status != Status::rate_limited) {
        refusals_ = 0;
        until_.reset();
        return std::nullopt;
    }
    std::chrono::seconds wait = kRateLimitFirstWait;
    for (int i = 0; i < refusals_ && wait < kRateLimitMaxWait; ++i) {
        wait *= 2;
    }
    wait = std::min<std::chrono::seconds>(wait, kRateLimitMaxWait);
    ++refusals_;
    until_ = now + wait;
    return until_;
}

}  // namespace meerkat::core
