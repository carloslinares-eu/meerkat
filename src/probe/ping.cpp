/**
 * @file ping.cpp
 * @brief Implementation of the ping probe.
 */
#include "probe/ping.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <exception>

#include "net/detail/text.hpp"

namespace meerkat::probe {

namespace {

/**
 * @brief Parses an unsigned integer at the start of @p s.
 * @param s The text.
 * @return The number, or -1 if @p s doesn't start with digits.
 */
int leading_int(std::string_view s) {
    int v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return (ec == std::errc{} && ptr != s.data()) ? v : -1;
}

/**
 * @brief Finds the number that directly precedes @p word in @p line,
 *        e.g. 5 in `5 received` for word `received`.
 * @param line The line.
 * @param word The word after the number.
 * @return The number, or -1 if not found.
 */
int number_before(std::string_view line, std::string_view word) {
    const auto pos = line.find(word);
    if (pos == std::string_view::npos || pos < 2) {
        return -1;
    }
    std::size_t start = pos - 1;  // the space before the word
    while (start > 0 && line[start - 1] >= '0' && line[start - 1] <= '9') {
        --start;
    }
    return leading_int(line.substr(start, pos - 1 - start));
}

}  // namespace

core::PingResult parse_ping(std::string_view text) {
    core::PingResult r;
    bool have_stats = false;
    net::detail::for_each_line(text, [&](std::string_view line) {
        if (line.find("packets transmitted") != std::string_view::npos) {
            const int sent = leading_int(line);
            const int received = number_before(line, "received");
            if (sent >= 0 && received >= 0) {
                r.sent = sent;
                r.received = received;
                have_stats = true;
            }
        } else if ((line.starts_with("rtt ") || line.starts_with("round-trip ")) &&
                   line.find('=') != std::string_view::npos) {
            const auto values = line.substr(line.find('=') + 1);
            double mn = 0, avg = 0, mx = 0, mdev = 0;
            const std::string copy{values};
            if (std::sscanf(copy.c_str(), " %lf/%lf/%lf/%lf", &mn, &avg, &mx, &mdev) == 4) {
                r.rtt_min_ms = mn;
                r.rtt_avg_ms = avg;
                r.rtt_max_ms = mx;
                r.rtt_mdev_ms = mdev;
            }
        }
    });
    if (have_stats) {
        r.status = core::Status::ok;
    } else {
        r.error = "no ping statistics in the output";
    }
    return r;
}

bool is_valid_ping_target(std::string_view target) {
    if (target.empty() || target.size() > 253 || target.front() == '-') {
        return false;
    }
    return std::none_of(target.begin(), target.end(), [](char c) {
        return static_cast<unsigned char>(c) <= ' ' || c == '\x7f';
    });
}

core::PingResult ping(std::string_view target, const PingOptions& options, const sys::Runner& run) {
    using namespace std::chrono;
    core::PingResult r;
    r.target = std::string{target};
    if (!is_valid_ping_target(target)) {
        r.error = "invalid ping target";
        return r;
    }
    const int count = std::clamp(options.count, 1, 100);
    const auto interval = std::max(options.interval, milliseconds{200});
    const auto reply = std::clamp(options.reply_timeout, seconds{1}, seconds{30});
    // ping's own deadline: every request sent, plus time for the last reply.
    const auto deadline = ceil<seconds>(interval * (count - 1) + reply) + seconds{1};

    std::array<char, 32> interval_text{};
    std::snprintf(interval_text.data(), interval_text.size(), "%.3f",
                  static_cast<double>(interval.count()) / 1000.0);
    sys::ProcessResult r_proc;
    try {
        r_proc = run({.program = "ping",
                             .args = {"-n", "-c", std::to_string(count), "-i", interval_text.data(),
                                      "-W", std::to_string(reply.count()), "-w",
                                      std::to_string(deadline.count()), std::string{target}},
                             .timeout = duration_cast<milliseconds>(deadline + seconds{5}),
                             .env = {{"LC_ALL", "C"}}});
    } catch (const std::exception& e) {
        r.error = std::string{"cannot run ping: "} + e.what();
        return r;
    }

    switch (r_proc.outcome) {
        case sys::ProcessResult::Outcome::spawn_failed:
            r.status = r_proc.not_found ? core::Status::not_available : core::Status::error;
            r.error = r_proc.not_found ? "ping is not installed (Debian package: iputils-ping)"
                                       : r_proc.error;
            return r;
        case sys::ProcessResult::Outcome::timed_out:
            r.status = core::Status::timeout;
            r.error = "ping did not finish within its deadline";
            return r;
        case sys::ProcessResult::Outcome::signaled:
            r.error = "ping was killed by signal " + std::to_string(r_proc.signal);
            return r;
        case sys::ProcessResult::Outcome::exited:
            break;
    }

    // Exit 0: replies received; 1: no (or not all) replies; 2: other error.
    auto parsed = parse_ping(r_proc.out);
    if (r_proc.exit_code <= 1 && parsed.status == core::Status::ok) {
        parsed.target = r.target;
        return parsed;
    }
    const auto message =
        std::string{net::detail::trim(r_proc.err.empty() ? r_proc.out : r_proc.err)};
    if (message.find("Network is unreachable") != std::string::npos) {
        r.status = core::Status::no_network;
    }
    r.error = message.empty() ? "ping failed with exit code " + std::to_string(r_proc.exit_code)
                              : message;
    return r;
}

}  // namespace meerkat::probe
