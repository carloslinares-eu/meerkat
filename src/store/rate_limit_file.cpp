/**
 * @file rate_limit_file.cpp
 * @brief Implementation of the rate-limit state file.
 */
#include "store/rate_limit_file.hpp"

#include <stdexcept>
#include <string>

#include "core/text.hpp"
#include "store/state_file.hpp"
#include "sys/atomic_file.hpp"

namespace meerkat::store {

std::optional<core::RateLimitState> read_rate_limit(const std::filesystem::path& data_dir) {
    const auto path = data_dir / kRateLimitFile;
    const auto content = read_state_file(path);
    if (!content) {
        return std::nullopt;
    }
    const std::string_view text{*content};
    const auto space = text.find(' ');
    const auto until = space == std::string_view::npos ? std::nullopt : core::parse_iso8601(text.substr(0, space));
    const auto count = space == std::string_view::npos ? std::string_view{} : text.substr(space + 1);
    const int refusals = core::text::parse_int<int>(count).value_or(0);
    if (!until || refusals < 1) {
        throw std::runtime_error(path.string() + " doesn't hold a time and a count; meerkat replaces it "
                                 "after the next speed test");
    }
    return core::RateLimitState{.until = *until, .refusals = refusals};
}

void write_rate_limit(const std::filesystem::path& data_dir, const std::optional<core::RateLimitState>& state) {
    const auto path = data_dir / kRateLimitFile;
    if (!state) {
        remove_state_file(path);
        return;
    }
    sys::AtomicFile file{path};
    file.write(core::to_iso8601(state->until) + " " + std::to_string(state->refusals) + "\n");
    file.commit();
}

}  // namespace meerkat::store
