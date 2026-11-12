/**
 * @file run.hpp
 * @brief `meerkat run`: records until stopped.
 */
#pragma once

#include <functional>

#include "app/log.hpp"
#include "app/recorder.hpp"
#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/scheduler.hpp"
#include "sys/sleep_inhibitor.hpp"

namespace meerkat::app {

/// Exit codes of `meerkat run`.
enum RunExit : int {
    kRunStopped = 0,    ///< Stopped by a signal (or the test hook).
    kRunFailed = 1,     ///< Couldn't start (data directory, files) or the clock failed.
};

/// What run_service() uses; replaceable in tests.
struct RunEnv {
    RecorderProbes probes;                 ///< How to measure.
    core::Clock* clock = nullptr;          ///< The scheduler's clock; must be set.
    sys::InhibitCall inhibit;              ///< Asks logind for the sleep lock; empty: don't.
    /// Handle SIGINT, SIGTERM and SIGHUP (stop; a second one exits at once).
    /// Tests turn it off.
    bool handle_signals = true;
    /// Called once the scheduler is about to run, with it; tests stop it from here.
    std::function<void(core::Scheduler&)> on_started;
};

/**
 * @brief Records until stopped: the connectivity probe and the speed test
 *        at their intervals, the daily manifest just after midnight UTC.
 *
 * In order: creates the data directory (mode 0750) if needed, opens the
 * files, takes the logind sleep lock (a warning if refused), warns about
 * the connection, seals the previous day, then runs the scheduler until
 * SIGINT, SIGTERM or SIGHUP. A running speed test finishes first; a second
 * signal exits at once (the files stay consistent: every row is synced).
 *
 * Signals are blocked in every thread and taken by a dedicated thread with
 * `sigtimedwait`, which calls Scheduler::stop() (that isn't safe in a
 * signal handler). Programs meerkat starts get the default signal setup.
 *
 * @param config A validated config.
 * @param env    Probes, clock and hooks.
 * @param log    Where to log.
 * @return kRunStopped, or kRunFailed.
 */
int run_service(const core::Config& config, RunEnv env, Log& log);

}  // namespace meerkat::app
