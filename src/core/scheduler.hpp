/**
 * @file scheduler.hpp
 * @brief Runs jobs at a fixed rate, lined up with the wall clock.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "core/clock.hpp"
#include "core/time.hpp"

namespace meerkat::core {

/// A job that runs at a fixed rate.
struct Job {
    std::string name;                      ///< For events and logs, e.g. `speed`.
    std::chrono::seconds interval{60};     ///< Time between slots; > 0.
    /// Slots are at `offset + k * interval` seconds since the Unix epoch (UTC),
    /// so a 5-minute job runs at :00, :05, :10 … Must be in [0, interval).
    std::chrono::seconds offset{0};
    /// What to do; gets the slot it runs for. It may throw: the error is
    /// reported (SchedulerEvent::Kind::failed) and the schedule goes on.
    std::function<void(Timestamp slot)> run;
};

/// Something the scheduler reports instead of running a slot normally.
struct SchedulerEvent {
    /// What happened.
    enum class Kind {
        overrun,  ///< The previous run lasted past the next slot(s); they were skipped.
        late,     ///< Woke up a slot or more late (suspend, clock change); missed slots skipped.
        failed,   ///< The job threw; `detail` has the message.
    };
    std::string job;                       ///< Job::name.
    Kind kind = Kind::overrun;             ///< What happened.
    Timestamp slot{};                      ///< First skipped slot, or the slot that failed.
    std::int64_t count = 1;                ///< Number of slots skipped (1 for `failed`).
    std::string detail;                    ///< Exception text for `failed`; empty otherwise.
};

/// Receives scheduler events. It is called from the job threads, possibly
/// two at once, so it must be thread-safe; it should be quick, since the job
/// that reported waits for it. Exceptions it throws are ignored.
using SchedulerEventHandler = std::function<void(const SchedulerEvent&)>;

/**
 * @brief The first slot at or after @p t.
 * @param t        A wall-clock time.
 * @param interval Slot spacing; > 0.
 * @param offset   Slot offset from the epoch.
 * @return The slot; @p t itself if it lies exactly on a slot.
 */
Timestamp slot_at_or_after(WallTime t, std::chrono::seconds interval, std::chrono::seconds offset);

/**
 * @brief The last slot at or before @p t.
 * @param t        A wall-clock time.
 * @param interval Slot spacing; > 0.
 * @param offset   Slot offset from the epoch.
 * @return The slot.
 */
Timestamp slot_at_or_before(WallTime t, std::chrono::seconds interval, std::chrono::seconds offset);

/**
 * @brief Runs several jobs at a fixed rate, each on its own thread.
 *
 * Each job runs at its slots (see Job::offset), not "interval after the last
 * run", so the schedule doesn't drift. A job runs on its own thread, so a
 * long speed test never delays the connectivity probe. Per job:
 * - The first run is the first slot at or after run() is called.
 * - **Overrun**: if a run lasts past the next slot, the slots it covered
 *   are skipped (one SchedulerEvent::Kind::overrun event) and the job runs
 *   again at the first slot after it finished. Runs never overlap.
 * - **Late wake-up** (suspend, or the clock jumped forward), while sleeping
 *   or during a run: slots missed completely are skipped with one
 *   SchedulerEvent::Kind::late event; the most recent slot runs at once. A
 *   run counts as suspended or jumped, not overrun, when the wall clock
 *   moved over a second more than the monotonic clock (Clock::steady_now()).
 * - **Clock set back**: a small step (up to about one interval) never runs a
 *   slot twice; the job waits for the slot after the one it ran. After a
 *   bigger jump back it follows the new time instead of stalling for the
 *   size of the jump (slot times can then repeat; measurements keep their
 *   own timestamps).
 * - A run that throws is reported (SchedulerEvent::Kind::failed) and the
 *   next slot runs normally.
 *
 * stop() wakes every sleeping job at once. A job that is running finishes
 * its current run first (runs are short and have their own timeouts).
 */
class Scheduler {
public:
    /**
     * @brief Sets up the jobs; nothing runs until run().
     * @param jobs     The jobs.
     * @param clock    Wall clock; must outlive the scheduler.
     * @param on_event Receives skips and failures; may be empty.
     * @throws std::invalid_argument if there are no jobs, a job has no
     *         function, an interval isn't positive, an offset is outside
     *         [0, interval), or two jobs have the same name.
     */
    Scheduler(std::vector<Job> jobs, Clock& clock, SchedulerEventHandler on_event = {});

    Scheduler(const Scheduler&) = delete;             ///< Owns threads while running: not copyable.
    Scheduler& operator=(const Scheduler&) = delete;  ///< Owns threads while running: not copyable.

    /**
     * @brief Runs the jobs until stop() is called; blocks.
     *
     * If stop() was already called, it returns at once.
     * @throws std::logic_error if called a second time.
     * @throws std::exception (whatever the clock threw, or `std::system_error`
     *         if a thread can't be started) if scheduling itself failed;
     *         the other jobs are stopped first.
     */
    void run();

    /**
     * @brief Ends run(): sleeping jobs wake now, running ones finish first.
     *
     * Thread-safe, and may be called before or during run(), any number of
     * times.
     * @warning Not async-signal-safe: don't call it from a signal handler.
     *          Handle signals on a normal thread (e.g. with `sigwait`) and
     *          call stop() from there.
     */
    void stop();

    /// @return True once stop() has been called.
    [[nodiscard]] bool stopping() const { return stop_.load(); }

private:
    /**
     * @brief One job's loop: sleep to the slot, run, work out the next slot.
     * @param job The job.
     */
    void run_job(const Job& job);

    /**
     * @brief The slot to wait for after the clock moved back, given the time
     *        now and the last slot that ran.
     * @param job  The job.
     * @param now  Current wall time.
     * @param last Last slot run, if any.
     * @return The slot.
     */
    static Timestamp resync(const Job& job, WallTime now, const std::optional<Timestamp>& last);

    /**
     * @brief Passes @p e to the handler, ignoring exceptions.
     * @param e The event.
     */
    void emit(const SchedulerEvent& e);

    std::vector<Job> jobs_;              ///< The jobs.
    Clock& clock_;                       ///< Wall clock.
    SchedulerEventHandler on_event_;     ///< Event receiver.
    std::atomic<bool> stop_{false};      ///< Set by stop().
    std::atomic<bool> started_{false};   ///< run() was called.
    std::mutex error_mutex_;             ///< Guards error_.
    std::exception_ptr error_;           ///< First scheduling failure, rethrown by run().
};

}  // namespace meerkat::core
