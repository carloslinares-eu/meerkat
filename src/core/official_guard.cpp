/**
 * @file official_guard.cpp
 * @brief Implementation of the official-measurement guard.
 */
#include "core/official_guard.hpp"

#include "core/text.hpp"

namespace meerkat::core {

OfficialCheck OfficialGuard::check(const std::vector<std::string>& running,
                                   std::optional<Timestamp> pause_until, Timestamp now) {
    OfficialCheck c;
    Cause cause = Cause::none;
    if (!running.empty()) {
        c.pause = "the Breitbandmessung app is running (" + text::join(running, ", ") + ")";
        cause = Cause::app;
    } else if (pause_until && *pause_until > now) {
        c.pause = "paused with `meerkat pause` until " + to_iso8601(*pause_until);
        cause = Cause::manual;
    }

    if (c.pause != last_) {
        if (c.pause) {
            c.notice = "speed tests paused: " + *c.pause;
        } else if (last_) {
            c.notice = last_cause_ == Cause::app
                           ? std::string{"the Breitbandmessung app has stopped; speed tests resume"}
                           : std::string{"the pause is over; speed tests resume"};
        }
    }
    last_ = c.pause;
    last_cause_ = cause;
    return c;
}

}  // namespace meerkat::core
