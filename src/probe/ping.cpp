/**
 * @file ping.cpp
 * @brief Implementation of the ping probe.
 */
#include "probe/ping.hpp"

#include <algorithm>
#include <exception>
#include <vector>

#include "core/text.hpp"

namespace meerkat::probe {

namespace {

using core::text::leading_double;
using core::text::parse_int;
using core::text::trim;

/// Longest interval between echo requests meerkat will ask for.
constexpr std::chrono::milliseconds kMaxInterval{10'000};
/// Extra time the process gets beyond ping's own expected run time.
constexpr std::chrono::seconds kProcessSlack{5};

/**
 * @brief Finds the number that precedes @p word in @p line, e.g. 5 in
 *        `5 received` (iputils) or `5 packets received` (BusyBox) for word
 *        `received`.
 * @param line The line.
 * @param word The word after the number.
 * @return The number, or `std::nullopt` if not found.
 */
std::optional<int> number_before(std::string_view line, std::string_view word) {
    const auto pos = line.find(word);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    auto head = line.substr(0, pos);
    if (!head.ends_with(' ')) {
        return std::nullopt;
    }
    head.remove_suffix(1);
    if (head.ends_with(" packets")) {
        head.remove_suffix(8);
    }
    std::size_t start = head.size();
    while (start > 0 && head[start - 1] >= '0' && head[start - 1] <= '9') {
        --start;
    }
    return parse_int<int>(head.substr(start));
}

/**
 * @brief Splits @p s on '/'.
 * @param s The text.
 * @return The parts.
 */
std::vector<std::string_view> split_slash(std::string_view s) {
    std::vector<std::string_view> parts;
    while (true) {
        const auto slash = s.find('/');
        parts.push_back(s.substr(0, slash));
        if (slash == std::string_view::npos) {
            return parts;
        }
        s.remove_prefix(slash + 1);
    }
}

/**
 * @brief Formats milliseconds as seconds with three decimals, e.g. `0.200`.
 *
 * Done by hand rather than with printf, whose decimal separator depends on
 * the locale (`0,200` in German), which ping would reject.
 *
 * @param ms Milliseconds (non-negative).
 * @return The text.
 */
std::string seconds_text(std::chrono::milliseconds ms) {
    const auto v = ms.count();
    std::string frac = std::to_string(v % 1000);
    frac.insert(0, 3 - frac.size(), '0');
    return std::to_string(v / 1000) + "." + frac;
}

}  // namespace

core::PingResult parse_ping(std::string_view text) {
    core::PingResult r;
    bool have_stats = false;
    core::text::for_each_line(text, [&](std::string_view line) {
        if (line.find("packets transmitted") != std::string_view::npos) {
            const auto sent = parse_int<int>(line.substr(0, line.find(' ')));
            const auto received = number_before(line, "received");
            if (sent && received) {
                r.sent = *sent;
                r.received = *received;
                have_stats = true;
            }
        } else if ((line.starts_with("rtt ") || line.starts_with("round-trip ")) &&
                   line.find('=') != std::string_view::npos) {
            // iputils: "rtt min/avg/max/mdev = a/b/c/d ms";
            // BusyBox/BSD: "round-trip min/avg/max = a/b/c ms" (no mdev).
            const auto parts = split_slash(trim(line.substr(line.find('=') + 1)));
            if (parts.size() == 3 || parts.size() == 4) {
                const auto mn = leading_double(parts[0]);
                const auto avg = leading_double(parts[1]);
                const auto mx = leading_double(parts[2]);
                if (mn && avg && mx) {
                    r.rtt_min_ms = mn;
                    r.rtt_avg_ms = avg;
                    r.rtt_max_ms = mx;
                    if (parts.size() == 4) {
                        r.rtt_mdev_ms = leading_double(parts[3]);
                    }
                }
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
    return std::ranges::none_of(target, [](char c) {
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
    const auto interval = std::clamp(options.interval, milliseconds{200}, kMaxInterval);
    const auto reply = std::clamp(options.reply_timeout, seconds{1}, seconds{30});
    // No -w: with -w, iputils keeps sending past -c until it gets c replies,
    // which skews the loss figure. -c alone sends exactly `count` requests and
    // waits up to -W for the last reply; the process timeout bounds the rest.
    const auto expected = interval * (count - 1) + reply;

    sys::ProcessResult proc;
    try {
        proc = run({.program = "ping",
                    .args = {"-n", "-c", std::to_string(count), "-i", seconds_text(interval), "-W",
                             std::to_string(reply.count()), std::string{target}},
                    .timeout = duration_cast<milliseconds>(expected + kProcessSlack),
                    .env = {{"LC_ALL", "C"}}});
    } catch (const std::exception& e) {
        r.error = std::string{"cannot run ping: "} + e.what();
        return r;
    }

    switch (proc.outcome) {
        case sys::ProcessResult::Outcome::spawn_failed:
            r.status = proc.not_found ? core::Status::not_available : core::Status::error;
            r.error = proc.not_found ? "ping is not installed (Debian package: iputils-ping)"
                                     : proc.error;
            return r;
        case sys::ProcessResult::Outcome::timed_out:
            r.status = core::Status::timeout;
            r.error = "ping did not finish in time";
            return r;
        case sys::ProcessResult::Outcome::signaled:
            r.error = "ping was killed by signal " + std::to_string(proc.signal);
            return r;
        case sys::ProcessResult::Outcome::exited:
            break;
    }

    // Exit 0: replies received; 1: no (or not all) replies; 2: an error. Even
    // with exit 2 ping may have printed valid statistics (e.g. some sends were
    // refused); those are kept, with the error text attached.
    const auto stderr_text = std::string{trim(proc.err)};
    auto parsed = parse_ping(proc.out);
    if (parsed.status == core::Status::ok) {
        parsed.target = r.target;
        if (proc.exit_code > 1) {
            parsed.error = stderr_text;
        }
        return parsed;
    }
    if (stderr_text.find("Network is unreachable") != std::string::npos ||
        stderr_text.find("Temporary failure in name resolution") != std::string::npos) {
        // No route, or (for a host name) no DNS server reachable: offline.
        r.status = core::Status::no_network;
    }
    r.error = !stderr_text.empty()
                  ? stderr_text
                  : parsed.error + " (exit code " + std::to_string(proc.exit_code) + ")";
    return r;
}

}  // namespace meerkat::probe
