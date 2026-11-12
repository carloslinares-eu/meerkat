/**
 * @file clock.hpp
 * @brief The wall clock the scheduler reads and sleeps on; replaceable in
 *        tests.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace meerkat::core {

/// A point in wall-clock time (UTC), with sub-second resolution.
using WallTime = std::chrono::system_clock::time_point;

/**
 * @brief Wall-clock time and sleeping, behind an interface so tests can run
 *        the scheduler without waiting.
 *
 * Implementations must be thread-safe: several job threads sleep at once.
 */
class Clock {
public:
    virtual ~Clock() = default;

    /// @return The current wall-clock time.
    [[nodiscard]] virtual WallTime now() = 0;

    /**
     * @brief Sleeps until now() reaches @p deadline, @p stop becomes true
     *        (see wake_all()), or earlier.
     *
     * May return early, so callers re-check now() and @p stop in a loop. The
     * system clock returns at least once a second, so a clock change or a
     * suspend/resume is noticed within a second.
     *
     * @param deadline When to wake up.
     * @param stop     Wakes the sleeper when set and followed by wake_all().
     */
    virtual void sleep_until(WallTime deadline, const std::atomic<bool>& stop) = 0;

    /**
     * @brief Wakes every sleep_until() so it re-checks its stop flag. Call it
     *        after setting the flag.
     */
    virtual void wake_all() = 0;

protected:
    /// Only implementations construct a clock.
    Clock() = default;
    /// Copying is up to the implementation.
    Clock(const Clock&) = default;
    /**
     * @brief Copying is up to the implementation.
     * @return This clock.
     */
    Clock& operator=(const Clock&) = default;
};

/**
 * @brief The real clock: `std::chrono::system_clock`, sleeping on a
 *        condition variable in slices of at most one second.
 */
class SystemClock final : public Clock {
public:
    SystemClock() = default;
    SystemClock(const SystemClock&) = delete;             ///< Holds a mutex: not copyable.
    SystemClock& operator=(const SystemClock&) = delete;  ///< Holds a mutex: not copyable.

    /// @copydoc Clock::now
    [[nodiscard]] WallTime now() override { return std::chrono::system_clock::now(); }

    /// @copydoc Clock::sleep_until
    void sleep_until(WallTime deadline, const std::atomic<bool>& stop) override;

    /// @copydoc Clock::wake_all
    void wake_all() override;

private:
    std::mutex mutex_;             ///< Guards the wait.
    std::condition_variable cv_;   ///< Signalled by wake_all().
};

}  // namespace meerkat::core
