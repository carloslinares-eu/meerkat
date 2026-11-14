/**
 * @file run.cpp
 * @brief Implementation of `meerkat run`.
 */
#include "app/run.hpp"

#include <pthread.h>
#include <signal.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <mutex>
#include <thread>

#include "meerkat/version.hpp"
#include "sys/process.hpp"

namespace meerkat::app {

namespace {

/// Exit code after a second signal (128 + SIGINT, as shells report it).
constexpr int kForcedExit = 130;

/// Sent to the signal thread by run_service() to end it.
constexpr int kWakeSignal = SIGUSR1;

/**
 * @brief The signals the signal thread waits for: the stop signals
 *        (SIGINT, SIGTERM, SIGHUP) and kWakeSignal.
 * @return The set.
 */
sigset_t handled_signals() {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGINT);
    ::sigaddset(&set, SIGTERM);
    ::sigaddset(&set, SIGHUP);
    ::sigaddset(&set, kWakeSignal);
    return set;
}

/**
 * @brief Takes SIGINT/SIGTERM/SIGHUP on a thread of its own for as long as
 *        it lives.
 *
 * The constructor blocks the signals in the calling thread, so every thread
 * started later inherits the block and only this one receives them. The
 * first stop signal sets a flag and stops the scheduler once there is one
 * (attach()); a second one kills the running programs and exits. The
 * destructor wakes the thread with kWakeSignal, joins it, takes any signal
 * still pending (so restoring the old mask can't kill the process), and
 * restores the mask.
 */
class SignalThread {
public:
    /**
     * @brief Blocks the signals and starts the thread.
     * @param log Where to log.
     * @throws std::system_error if the thread can't be started (the mask is
     *         restored).
     */
    explicit SignalThread(Log& log) : log_{log} {
        const sigset_t set = handled_signals();
        ::pthread_sigmask(SIG_BLOCK, &set, &old_);
        try {
            thread_ = std::thread([this] { loop(); });
        } catch (...) {
            ::pthread_sigmask(SIG_SETMASK, &old_, nullptr);
            throw;
        }
    }

    /// Ends the thread, drops pending signals and restores the mask.
    ~SignalThread() {
        done_.store(true);
        ::pthread_kill(thread_.native_handle(), kWakeSignal);
        thread_.join();
        const sigset_t set = handled_signals();
        const timespec zero{0, 0};
        while (::sigtimedwait(&set, nullptr, &zero) > 0) {
        }
        ::pthread_sigmask(SIG_SETMASK, &old_, nullptr);
    }

    SignalThread(const SignalThread&) = delete;             ///< Owns a thread: not copyable.
    SignalThread& operator=(const SignalThread&) = delete;  ///< Owns a thread: not copyable.

    /**
     * @brief From now on a stop signal stops @p scheduler; stops it at once
     *        if one already came.
     * @param scheduler The scheduler; must outlive this object or be detached.
     */
    void attach(core::Scheduler* scheduler) {
        std::lock_guard lock{mutex_};
        scheduler_ = scheduler;
        if (scheduler_ != nullptr && stop_requested_) {
            scheduler_->stop();
        }
    }

    /// @return True once a stop signal came.
    [[nodiscard]] bool stop_requested() {
        std::lock_guard lock{mutex_};
        return stop_requested_;
    }

private:
    /// Waits for signals until the destructor wakes it.
    void loop() {
        const sigset_t set = handled_signals();
        int received = 0;
        while (true) {
            int sig = 0;
            if (::sigwait(&set, &sig) != 0) {
                continue;
            }
            if (sig == kWakeSignal) {
                if (done_.load()) {
                    return;
                }
                continue;
            }
            if (++received == 1) {
                log_.notice(std::string{"stopping (SIG"} + ::sigabbrev_np(sig) +
                            "); a running speed test finishes first");
                std::lock_guard lock{mutex_};
                stop_requested_ = true;
                if (scheduler_ != nullptr) {
                    scheduler_->stop();
                }
            } else {
                log_.warning("second signal: exiting now");
                sys::kill_running_children();  // don't leave a speed test running
                log_.clear_status();
                std::_Exit(kForcedExit);
            }
        }
    }

    Log& log_;                               ///< Where to log.
    sigset_t old_{};                         ///< Mask before.
    std::thread thread_;                     ///< The waiting thread.
    std::atomic<bool> done_{false};          ///< Set by the destructor.
    std::mutex mutex_;                       ///< Guards the two below.
    core::Scheduler* scheduler_ = nullptr;   ///< What to stop.
    bool stop_requested_ = false;            ///< A stop signal came.
};

/**
 * @brief Creates the data directory (mode 0750) if it doesn't exist.
 * @param dir The directory.
 * @param log Where to log.
 * @return True if it exists now.
 */
bool ensure_data_dir(const std::filesystem::path& dir, Log& log) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        return true;
    }
    if (!fs::create_directories(dir, ec) && ec) {
        log.error("can't create the data directory " + dir.string() + ": " + ec.message());
        return false;
    }
    fs::permissions(dir, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec,
                    fs::perm_options::replace, ec);
    log.notice("created the data directory " + dir.string());
    return true;
}

/**
 * @brief Logs a scheduler event.
 * @param log Where to log.
 * @param e   The event.
 */
void log_event(Log& log, const core::SchedulerEvent& e) {
    const auto n = std::to_string(e.count);
    switch (e.kind) {
    case core::SchedulerEvent::Kind::overrun:
        log.warning(e.job + ": the previous run took too long; skipped " + n +
                    " slot(s) from " + core::to_iso8601(e.slot));
        break;
    case core::SchedulerEvent::Kind::late:
        log.warning(e.job + ": woke up late (suspend or clock change?); skipped " + n +
                    " slot(s) from " + core::to_iso8601(e.slot));
        break;
    case core::SchedulerEvent::Kind::failed:
        log.error(e.job + " failed at " + core::to_iso8601(e.slot) + ": " + e.detail);
        break;
    case core::SchedulerEvent::Kind::clock_back:
        log.warning(e.job + ": the clock went back; waiting for " + core::to_iso8601(e.slot) +
                    " so that no slot runs twice (" + n + " slot(s) already done)");
        break;
    case core::SchedulerEvent::Kind::clock_back_repeat:
        log.warning(e.job + ": the clock went back too far to wait; going on from " +
                    core::to_iso8601(e.slot) + ", so " + n + " slot time(s) repeat");
        break;
    }
}

}  // namespace

int run_service(const core::Config& config, RunEnv env, Log& log) {
    using std::chrono::seconds;
    // First of all, before any other thread exists: from here on Ctrl+C is
    // handled, even while start-up waits for logind.
    std::optional<SignalThread> signals;
    if (env.handle_signals) {
        try {
            signals.emplace(log);
        } catch (const std::exception& e) {
            log.error(std::string{"can't start the signal thread: "} + e.what());
            return kRunFailed;
        }
    }
    const auto stop_requested = [&signals] { return signals && signals->stop_requested(); };

    log.notice(std::string{"meerkat "} + std::string{kVersion} + " recording to " +
               config.data_dir.string() + ": speed test every " +
               core::format_duration(config.speed_interval) + ", connectivity every " +
               core::format_duration(config.connectivity_interval));
    if (!config.contract.download || !config.contract.upload) {
        log.warning("no contract figures in the config: the data is recorded, but `meerkat "
                    "report` will have nothing to compare it with");
    }
    if (!ensure_data_dir(config.data_dir, log)) {
        return kRunFailed;
    }

    std::unique_ptr<Recorder> recorder;
    try {
        recorder = std::make_unique<Recorder>(config, env.probes, log);
    } catch (const DataDirBusy& e) {
        log.error(e.what());  // the reason as it is: not an I/O problem
        return kRunFailed;
    } catch (const std::exception& e) {
        log.error(std::string{"can't open the data files: "} + e.what());
        return kRunFailed;
    }

    sys::SleepInhibitor lock;
    if (env.inhibit && !stop_requested()) {
        try {
            lock = sys::SleepInhibitor::acquire({}, env.inhibit);
            log.notice("holding the logind lock against suspend (" + sys::InhibitRequest{}.what +
                       ")");
        } catch (const std::exception& e) {
            log.warning(std::string{"the computer may suspend and miss measurements: "} +
                        e.what());
        }
    }
    if (!stop_requested()) {
        recorder->startup();
    }

    int rc = kRunStopped;
    try {
        std::vector<core::Job> jobs;
        jobs.push_back({"connectivity", config.connectivity_interval, seconds{0},
                        [&recorder](core::Timestamp) { recorder->connectivity_round(); }});
        jobs.push_back({"speed", config.speed_interval, seconds{0},
                        [&recorder](core::Timestamp) { recorder->speed_round(); }});
        // Just after midnight UTC, so the day's last rows are in.
        jobs.push_back({"manifest", std::chrono::days{1}, std::chrono::minutes{10},
                        [&recorder](core::Timestamp) { recorder->seal_pending(); }});
        core::Scheduler scheduler{std::move(jobs), *env.clock,
                                  [&log](const core::SchedulerEvent& e) { log_event(log, e); }};
        if (signals) {
            signals->attach(&scheduler);
        }
        if (env.on_started) {
            env.on_started(scheduler);
        }
        try {
            scheduler.run();
        } catch (const std::exception& e) {
            log.error(std::string{"scheduling failed: "} + e.what());
            rc = kRunFailed;
        }
        if (signals) {
            signals->attach(nullptr);
        }
    } catch (const std::exception& e) {
        log.error(std::string{"can't start the scheduler: "} + e.what());
        rc = kRunFailed;
    }
    recorder->finish();
    log.clear_status();
    log.notice("stopped");
    return rc;
}

}  // namespace meerkat::app
