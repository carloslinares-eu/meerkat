/**
 * @file limit.cpp
 * @brief The speed-test service's limit for `run` and `once`.
 */
#include "app/limit.hpp"

#include <exception>
#include <utility>

#include "store/rate_limit_file.hpp"

namespace meerkat::app {

std::optional<core::RateLimitState> load_rate_limit(const std::filesystem::path& data_dir,
                                                    const std::function<void(const std::string&)>& warn) {
    try {
        return store::read_rate_limit(data_dir);
    } catch (const std::exception& e) {
        warn(e.what());
        return std::nullopt;
    }
}

core::SpeedResult rate_limit_skip(std::string backend, core::Timestamp until) {
    core::SpeedResult r;
    r.backend = std::move(backend);
    r.status = core::Status::rate_limited;
    r.error = core::not_started("waiting for the speed-test service's limit until " + core::to_iso8601(until));
    return r;
}

std::optional<core::RateLimitState> update_rate_limit(const std::filesystem::path& data_dir,
                                                      const std::optional<core::RateLimitState>& before,
                                                      core::Status status, core::Timestamp started,
                                                      const std::function<void(const std::string&)>& warn) {
    const auto after = core::rate_limit_after(before, status, started);
    if (after != before) {
        try {
            store::write_rate_limit(data_dir, after);
        } catch (const std::exception& e) {
            warn(e.what());
        }
    }
    return after;
}

}  // namespace meerkat::app
