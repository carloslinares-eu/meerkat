/**
 * @file clock.cpp
 * @brief Implementation of the system clock.
 */
#include "core/clock.hpp"

#include <algorithm>

namespace meerkat::core {

void SystemClock::sleep_until(WallTime deadline, const std::atomic<bool>& stop) {
    // Sleep against the steady clock in short slices: the wall clock can jump
    // (NTP, manual change) or stand still during suspend, and the caller
    // re-reads it after every slice.
    constexpr std::chrono::milliseconds kMaxSlice{1000};
    const auto remaining = deadline - now();
    if (remaining <= WallTime::duration::zero()) {
        return;
    }
    const auto slice = std::min<std::chrono::steady_clock::duration>(
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining), kMaxSlice);
    std::unique_lock lock{mutex_};
    cv_.wait_for(lock, slice, [&stop] { return stop.load(); });
}

void SystemClock::wake_all() {
    // Taking the mutex orders this after the caller's stop.store(), so a
    // sleeper either sees the flag in its predicate or gets the notification.
    { std::lock_guard lock{mutex_}; }
    cv_.notify_all();
}

}  // namespace meerkat::core
