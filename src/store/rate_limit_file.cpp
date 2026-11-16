/**
 * @file rate_limit_file.cpp
 * @brief Implementation of the rate-limit state file.
 */
#include "store/rate_limit_file.hpp"

#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <stdexcept>
#include <string>
#include <system_error>

#include "store/append_file.hpp"
#include "sys/atomic_file.hpp"
#include "sys/regular_file.hpp"

namespace meerkat::store {

std::optional<core::RateLimitState> read_rate_limit(const std::filesystem::path& data_dir) {
    const auto path = data_dir / kRateLimitFile;
    std::string content;
    try {
        content = sys::read_regular_file(path, 64);
    } catch (const std::system_error& e) {
        if (e.code() == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }
        throw;
    }
    std::string_view text{content};
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }
    const auto space = text.find(' ');
    const auto until = space == std::string_view::npos ? std::nullopt : core::parse_iso8601(text.substr(0, space));
    int refusals = 0;
    const auto count = space == std::string_view::npos ? std::string_view{} : text.substr(space + 1);
    const auto [end, ec] = std::from_chars(count.data(), count.data() + count.size(), refusals);
    if (!until || ec != std::errc{} || end != count.data() + count.size() || refusals < 1) {
        throw std::runtime_error(path.string() + " doesn't hold a time and a count; meerkat replaces it "
                                 "after the next speed test");
    }
    return core::RateLimitState{.until = *until, .refusals = refusals};
}

void write_rate_limit(const std::filesystem::path& data_dir, const std::optional<core::RateLimitState>& state) {
    const auto path = data_dir / kRateLimitFile;
    if (!state) {
        if (::unlink(path.c_str()) == 0) {
            sync_parent_dir(path);
        } else if (errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(), "can't remove " + path.string());
        }
        return;
    }
    sys::AtomicFile file{path};
    file.write(core::to_iso8601(state->until) + " " + std::to_string(state->refusals) + "\n");
    file.commit();
}

}  // namespace meerkat::store
