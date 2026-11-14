/**
 * @file official.cpp
 * @brief Implementation of the official-measurement checks shared by
 *        `meerkat run` and `meerkat once`.
 */
#include "app/official.hpp"

#include <exception>
#include <utility>

#include "store/pause_file.hpp"

namespace meerkat::app {

OfficialWatch::OfficialWatch(std::vector<std::string> programs, std::filesystem::path data_dir,
                             ProcessLister processes, Sink notice, Sink warning)
    : programs_{std::move(programs)},
      data_dir_{std::move(data_dir)},
      processes_{std::move(processes)},
      notice_{std::move(notice)},
      warning_{std::move(warning)} {}

std::optional<std::string> OfficialWatch::check(core::Timestamp now) {
    std::vector<std::string> running;
    if (processes_ && !programs_.empty()) {
        running = sys::running_programs(processes_(), programs_);
    }
    std::optional<core::Timestamp> until;
    std::string pause_error;
    if (!data_dir_.empty()) {
        try {
            until = store::read_pause(data_dir_);
        } catch (const std::exception& e) {
            pause_error = e.what();
        }
    }
    std::lock_guard lock{mutex_};
    if (pause_error != last_pause_error_) {
        if (!pause_error.empty() && warning_) {
            warning_("the pause file is ignored: " + pause_error);
        }
        last_pause_error_ = pause_error;
    }
    auto decision = guard_.check(running, until, now);
    if (decision.notice && notice_) {
        notice_(*decision.notice);
    }
    return std::move(decision.pause);
}

core::SpeedResult official_paused(std::string why, std::string backend) {
    core::SpeedResult r;  // a partial run is no evidence: no figures
    r.status = core::Status::paused_official;
    r.error = std::move(why);
    r.backend = std::move(backend);
    return r;
}

core::SpeedResult watched_speed_test(
    const std::function<core::SpeedResult(const sys::StopCheck&)>& speed, OfficialWatch& watch,
    const std::function<core::Timestamp()>& now) {
    std::optional<std::string> stopped_for;
    auto last = now();
    const sys::StopCheck stop = [&] {
        const auto t = now();
        if (t - last < kOfficialPoll && t >= last) {
            return false;
        }
        last = t;
        stopped_for = watch.check(t);
        return stopped_for.has_value();
    };
    auto result = speed(stop);
    if (result.stopped && stopped_for && result.status != core::Status::ok) {
        return official_paused("stopped: " + *stopped_for, std::move(result.backend));
    }
    return result;
}

}  // namespace meerkat::app
