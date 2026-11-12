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
#include <thread>

#include "meerkat/version.hpp"
#include "sys/process.hpp"

namespace meerkat::app {

namespace {

/// Exit code after a second signal (128 + SIGINT, as shells report it).
constexpr int kForcedExit = 130;

/**
 * @brief The signals that stop meerkat.
 * @return SIGINT, SIGTERM and SIGHUP.
 */
sigset_t stop_signals() {
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGINT);
    ::sigaddset(&set, SIGTERM);
    ::sigaddset(&set, SIGHUP);
    return set;
}

/**
 * @brief Blocks the stop signals in this thread (and so in every thread
 *        started from it) for its lifetime, then restores the old mask.
 */
class SignalBlock {
public:
    /// Blocks them.
    SignalBlock() {
        const sigset_t set = stop_signals();
        ::pthread_sigmask(SIG_BLOCK, &set, &old_);
    }
    /// Restores the previous mask.
    ~SignalBlock() { ::pthread_sigmask(SIG_SETMASK, &old_, nullptr); }
    SignalBlock(const SignalBlock&) = delete;             ///< Not copyable.
    SignalBlock& operator=(const SignalBlock&) = delete;  ///< Not copyable.

private:
    sigset_t old_{};  ///< Mask before.
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
    }
}

}  // namespace

int run_service(const core::Config& config, RunEnv env, Log& log) {
    using std::chrono::seconds;
    // Block before any thread exists, so every thread inherits the mask and
    // only the signal thread below receives the signals.
    std::optional<SignalBlock> block;
    if (env.handle_signals) {
        block.emplace();
    }

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
    } catch (const std::exception& e) {
        log.error(std::string{"can't open the data files: "} + e.what());
        return kRunFailed;
    }

    sys::SleepInhibitor lock;
    if (env.inhibit) {
        try {
            lock = sys::SleepInhibitor::acquire({}, env.inhibit);
            log.notice("holding the logind lock against suspend (" + sys::InhibitRequest{}.what +
                       ")");
        } catch (const std::exception& e) {
            log.warning(std::string{"the computer may suspend and miss measurements: "} +
                        e.what());
        }
    }

    recorder->startup();

    std::vector<core::Job> jobs;
    jobs.push_back({"connectivity", config.connectivity_interval, seconds{0},
                    [&recorder](core::Timestamp) { recorder->connectivity_round(); }});
    jobs.push_back({"speed", config.speed_interval, seconds{0},
                    [&recorder](core::Timestamp) { recorder->speed_round(); }});
    // Just after midnight UTC, so the day's last rows are in.
    jobs.push_back({"manifest", std::chrono::days{1}, std::chrono::minutes{10},
                    [&recorder](core::Timestamp) { recorder->seal_previous_day(); }});
    core::Scheduler scheduler{std::move(jobs), *env.clock,
                              [&log](const core::SchedulerEvent& e) { log_event(log, e); }};

    std::atomic<bool> done{false};
    std::thread signals;
    if (env.handle_signals) {
        signals = std::thread([&] {
            const sigset_t set = stop_signals();
            const timespec tick{0, 200'000'000};
            int received = 0;
            while (!done.load()) {
                siginfo_t info{};
                const int sig = ::sigtimedwait(&set, &info, &tick);
                if (sig < 0) {
                    continue;  // timeout or EINTR: check `done` again
                }
                if (++received == 1) {
                    log.notice(std::string{"stopping ("} + ::sigabbrev_np(sig) +
                               "); a running speed test finishes first");
                    scheduler.stop();
                } else {
                    log.warning("second signal: exiting now");
                    sys::kill_running_children();  // don't leave a speed test running
                    log.clear_status();
                    std::_Exit(kForcedExit);
                }
            }
        });
    }

    int rc = kRunStopped;
    if (env.on_started) {
        env.on_started(scheduler);
    }
    try {
        scheduler.run();
    } catch (const std::exception& e) {
        log.error(std::string{"scheduling failed: "} + e.what());
        rc = kRunFailed;
    }
    done.store(true);
    if (signals.joinable()) {
        signals.join();
    }
    recorder->finish();
    log.clear_status();
    log.notice("stopped");
    return rc;
}

}  // namespace meerkat::app
