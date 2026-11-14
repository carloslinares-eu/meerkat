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

OfficialInputs read_official(const std::vector<std::string>& programs,
                             const std::filesystem::path& data_dir,
                             const std::function<std::vector<sys::ProcessEntry>()>& processes) {
    OfficialInputs in;
    if (processes && !programs.empty()) {
        in.running = sys::running_programs(processes(), programs);
    }
    if (!data_dir.empty()) {
        try {
            in.pause_until = store::read_pause(data_dir);
        } catch (const std::exception& e) {
            in.pause_error = e.what();
        }
    }
    return in;
}

sys::StopCheck polled_stop(std::function<core::Timestamp()> now, core::Timestamp start,
                           std::chrono::seconds poll, std::function<bool()> ask) {
    return [now = std::move(now), last = start, poll, ask = std::move(ask)]() mutable {
        const auto t = now();
        if (t - last < poll && t >= last) {
            return false;
        }
        last = t;
        return ask();
    };
}

}  // namespace meerkat::app
