/**
 * @file outage.cpp
 * @brief Implementation of round assessment and the outage tracker.
 */
#include "core/outage.hpp"

#include <stdexcept>
#include <utility>

namespace meerkat::core {

namespace {

/// What one ping says.
enum class PingVerdict {
    reply,     ///< At least one reply.
    failed,    ///< Clearly no answer.
    no_route,  ///< No route at all.
    unclear,   ///< Says nothing about the line.
};

/**
 * @brief Classifies one ping.
 * @param p The ping.
 * @return The verdict.
 */
PingVerdict verdict(const PingResult& p) {
    switch (p.status) {
    case Status::ok:
        if (!p.received) {
            return PingVerdict::unclear;
        }
        return *p.received > 0 ? PingVerdict::reply : PingVerdict::failed;
    case Status::no_network:
        return PingVerdict::no_route;
    case Status::timeout:
        return PingVerdict::failed;
    default:
        return PingVerdict::unclear;
    }
}

/**
 * @brief A short reason for a failed ping, e.g. `1.1.1.1: no reply`.
 * @param p The ping.
 * @return The reason.
 */
std::string ping_reason(const PingResult& p) {
    std::string why;
    switch (verdict(p)) {
    case PingVerdict::failed: why = p.status == Status::timeout ? "timed out" : "no reply"; break;
    case PingVerdict::no_route: why = "no route"; break;
    default: why = p.error.empty() ? std::string{to_string(p.status)} : p.error; break;
    }
    return p.target + ": " + why;
}

}  // namespace

std::string to_string(OutageScope s) {
    switch (s) {
    case OutageScope::internet: return "internet";
    case OutageScope::local: return "local";
    case OutageScope::unknown: break;
    }
    return "unknown";
}

std::string to_string(OutageEnd e) {
    switch (e) {
    case OutageEnd::recovered: return "recovered";
    case OutageEnd::gap: return "gap";
    case OutageEnd::stopped: break;
    }
    return "stopped";
}

RoundAssessment assess(const ConnectivityRound& round) {
    RoundAssessment a;
    bool any_failed = false;
    bool no_route = false;
    std::string reasons;
    for (const auto& p : round.hosts) {
        const auto v = verdict(p);
        if (v == PingVerdict::reply) {
            return {Reachability::up, OutageScope::unknown, {}};
        }
        any_failed = any_failed || v == PingVerdict::failed || v == PingVerdict::no_route;
        no_route = no_route || v == PingVerdict::no_route;
        reasons += (reasons.empty() ? "" : "; ") + ping_reason(p);
    }

    if (round.hosts.empty()) {
        if (round.dns && round.dns->status == Status::ok) {
            return {Reachability::up, OutageScope::unknown, {}};
        }
        // No DNS server answered (probe::resolve() maps EAI_AGAIN to it).
        const bool temporary = round.dns && round.dns->status == Status::no_network;
        if (round.dns && round.dns->status == Status::timeout) {
            any_failed = true;
            reasons = "DNS lookup of " + round.dns->host + " timed out";
        } else if (temporary) {
            any_failed = true;
            reasons = "DNS lookup of " + round.dns->host + ": " + round.dns->error;
        } else {
            reasons = round.dns ? "DNS lookup of " + round.dns->host + ": " +
                                      (round.dns->error.empty() ? std::string{to_string(round.dns->status)}
                                                                : round.dns->error)
                                : "nothing was probed";
        }
    }

    a.reason = reasons;
    if (!any_failed) {
        a.reach = Reachability::unknown;
        return a;
    }
    a.reach = Reachability::down;
    // The gateway answering beats a "no route" from a host name that
    // couldn't be resolved: the router is there, what's beyond it isn't.
    const auto gw = round.gateway ? verdict(*round.gateway) : PingVerdict::unclear;
    if (gw == PingVerdict::reply) {
        a.scope = OutageScope::internet;
    } else if (no_route || gw == PingVerdict::failed || gw == PingVerdict::no_route) {
        a.scope = OutageScope::local;
    }
    return a;
}

std::optional<std::chrono::seconds> Outage::duration() const {
    if (!end) {
        return std::nullopt;
    }
    return *end > start ? *end - start : std::chrono::seconds{0};
}

OutageSettings OutageSettings::for_interval(std::chrono::seconds interval, int outage_after) {
    OutageSettings s;
    s.failures_to_start = outage_after;
    s.max_gap = 3 * interval;
    return s;
}

OutageTracker::OutageTracker(OutageSettings settings) : settings_{settings} {
    if (settings_.failures_to_start < 1 || settings_.successes_to_end < 1) {
        throw std::invalid_argument("OutageTracker: thresholds must be at least 1");
    }
    if (settings_.max_gap <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("OutageTracker: max_gap must be positive");
    }
}

std::vector<OutageEvent> OutageTracker::observe(const ConnectivityRound& round) {
    return observe(round.time, assess(round));
}

OutageEvent OutageTracker::close(Timestamp end, OutageEnd reason) {
    Outage o = std::move(*open_);
    open_.reset();
    ups_ = 0;
    o.end = end;
    o.end_reason = reason;
    return {OutageEvent::Kind::ended, std::move(o)};
}

std::vector<OutageEvent> OutageTracker::observe(Timestamp time, const RoundAssessment& a) {
    std::vector<OutageEvent> events;

    if (a.reach == Reachability::unknown) {
        return events;  // nothing was observed
    }
    // A long pause since the last known round: nothing is known about the
    // time in between. A step back (clock change) isn't a gap.
    if (last_ && time > *last_ && time - *last_ > settings_.max_gap) {
        if (open_) {
            // Recovery wasn't confirmed: the outage is only known to have
            // lasted until its last down round.
            events.push_back(close(last_down_, OutageEnd::gap));
        }
        downs_ = 0;
    }
    last_ = time;

    switch (a.reach) {
    case Reachability::unknown:
    case Reachability::inconclusive:
        break;
    case Reachability::down:
        ups_ = 0;
        if (open_) {
            ++open_->down_rounds;
            last_down_ = time;
            if (open_->scope == OutageScope::unknown) {
                open_->scope = a.scope;
            }
            break;
        }
        if (downs_ == 0) {
            pending_ = Outage{};
            pending_.start = time;
            pending_.reason = a.reason;
        }
        ++downs_;
        if (pending_.scope == OutageScope::unknown) {
            pending_.scope = a.scope;
        }
        if (downs_ >= settings_.failures_to_start) {
            pending_.down_rounds = downs_;
            open_ = pending_;
            last_down_ = time;
            downs_ = 0;
            events.push_back({OutageEvent::Kind::started, *open_});
        }
        break;
    case Reachability::up:
        downs_ = 0;
        if (open_) {
            if (ups_ == 0) {
                first_up_ = time;
            }
            ++ups_;
            if (ups_ >= settings_.successes_to_end) {
                events.push_back(close(first_up_, OutageEnd::recovered));
            }
        }
        break;
    }
    return events;
}

std::optional<OutageEvent> OutageTracker::finish() {
    downs_ = 0;
    if (!open_) {
        return std::nullopt;
    }
    return close(last_down_, OutageEnd::stopped);
}

}  // namespace meerkat::core
