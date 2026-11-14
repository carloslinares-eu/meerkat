/**
 * @file outage.hpp
 * @brief Turns connectivity probe rounds into outage start and end events.
 */
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "core/measurement.hpp"
#include "core/time.hpp"

namespace meerkat::core {

/// The results of one connectivity probe round (all taken at about the same time).
struct ConnectivityRound {
    Timestamp time{};                     ///< When the round started (UTC).
    std::vector<PingResult> hosts;        ///< Pings to the public hosts (`ping_hosts`).
    std::optional<PingResult> gateway;    ///< Ping to the default gateway, if it was pinged.
    std::optional<DnsResult> dns;         ///< The DNS lookup, if it ran.
};

/// Whether the internet was reachable in a round.
enum class Reachability {
    up,       ///< At least one public host answered.
    down,     ///< No public host answered, and at least one clearly failed.
    unknown,  ///< Can't tell (e.g. `ping` missing or broken); doesn't count either way.
    /// Measured, but the result can't be trusted (pings lost while a speed
    /// test fills the line). Doesn't count either way, but unlike `unknown`
    /// it shows that meerkat was observing, so it bridges a gap.
    inconclusive,
};

/// Where an outage seems to be.
enum class OutageScope {
    internet,  ///< The router (gateway) answered but nothing beyond it: the line.
    local,     ///< No route, or the gateway didn't answer: router, cable or Wi-Fi.
    unknown,   ///< The gateway wasn't pinged, or its result is unclear.
};

/**
 * @brief The name of a scope, as stored and logged.
 * @param s The scope.
 * @return `internet`, `local` or `unknown`.
 */
std::string to_string(OutageScope s);

/// What one round says about the connection.
struct RoundAssessment {
    Reachability reach = Reachability::unknown;  ///< Up, down or unknown.
    OutageScope scope = OutageScope::unknown;    ///< Where, when `down`.
    std::string reason;                          ///< Why it counts as down or unknown; empty when up.
};

/**
 * @brief Decides whether a round saw the internet.
 *
 * A ping counts as a reply if it is `ok` with at least one packet received,
 * and as a clear failure if it is `ok` with none received, `no_network` or
 * `timeout`. Anything else (`error`, `not_available`, …) says nothing about
 * the line.
 *
 * - **up**: a public host replied.
 * - **down**: no public host replied and at least one clearly failed.
 * - Without public hosts (core::validate() doesn't allow that in a config,
 *   but rounds rebuilt from CSV may lack them), the DNS lookup decides:
 *   `ok` is up; `timeout` or
 *   a temporary resolver failure (`EAI_AGAIN`, e.g. the router answering
 *   SERVFAIL) is down; `no_network` is down with scope `local`, like a ping
 *   that found no route. It isn't used otherwise: the router may answer it
 *   from its cache while the line is down.
 * - **scope** of a down round: `internet` if the gateway replied (a host
 *   name that can't be resolved is then the line's fault too), `local` if
 *   the gateway clearly failed or a ping found no route, `unknown` otherwise.
 *
 * @param round The round.
 * @return The assessment.
 */
RoundAssessment assess(const ConnectivityRound& round);

/// When an outage starts and ends.
struct OutageSettings {
    /// Consecutive down rounds that make an outage (`[connectivity]
    /// outage_after`). 1 counts every single lost round.
    int failures_to_start = 2;
    /// Consecutive up rounds that end it.
    int successes_to_end = 1;
    /// A longer pause between two known (up or down) rounds (suspend,
    /// meerkat stopped, `ping` broken) means nothing was observed in
    /// between: an open outage is ended at its last down round
    /// (OutageEnd::gap) instead of being stretched over the gap, and a run of
    /// down rounds starts again. Use for_interval() to set it from the probe
    /// interval.
    std::chrono::seconds max_gap{90};

    /**
     * @brief Settings for a probe interval.
     *
     * max_gap is three intervals, or two intervals plus the longest a round
     * can take if that is more: during an outage the probes run into their
     * timeouts, the round outlasts its slot, the scheduler skips the next
     * one, and the round after it starts up to `round_time + interval`
     * later. That must not count as a gap.
     * @param interval     Time between connectivity rounds; > 0.
     * @param outage_after Down rounds that start an outage (failures_to_start).
     * @param round_time   The longest one round can take (all pings and the
     *                     DNS lookup running into their time limits).
     * @return The settings.
     */
    static OutageSettings for_interval(std::chrono::seconds interval, int outage_after,
                                       std::chrono::seconds round_time = std::chrono::seconds{0});
};

/// Why an outage ended.
enum class OutageEnd {
    recovered,  ///< The internet answered again (`end` is the first up round).
    /// No known round for longer than OutageSettings::max_gap, before
    /// recovery was confirmed. `end` is the first up round if one was seen
    /// (fewer than OutageSettings::successes_to_end), else the last down
    /// round.
    gap,
    /// The tracker was finished while it was open. `end` as for `gap`.
    stopped,
};

/**
 * @brief The name of an end reason, as stored and logged.
 * @param e The reason.
 * @return `recovered`, `gap` or `stopped`.
 */
std::string to_string(OutageEnd e);

/// One outage.
struct Outage {
    /// The first down round. The line went down between the last up round
    /// and this one, so the real start is up to one interval earlier.
    Timestamp start{};
    std::optional<Timestamp> end;            ///< Set once it ended; see `end_reason`.
    OutageEnd end_reason = OutageEnd::recovered;  ///< Why it ended, once `end` is set.
    OutageScope scope = OutageScope::unknown;     ///< The first known scope of its down rounds.
    int down_rounds = 0;                     ///< Down rounds seen in it.
    /// For `gap` and `stopped`: `end` is an up round, but fewer than
    /// OutageSettings::successes_to_end of them were seen.
    bool unconfirmed_end = false;
    std::string reason;                      ///< Why the first down round was down.

    /**
     * @brief How long it lasted.
     * @return `end - start` (0 if the clock went back), or nothing while open.
     */
    [[nodiscard]] std::optional<std::chrono::seconds> duration() const;
};

/// An outage started or ended.
struct OutageEvent {
    /// Which.
    enum class Kind {
        started,  ///< The outage reached OutageSettings::failures_to_start rounds.
        ended,    ///< It ended; Outage::end and Outage::end_reason are set.
    };
    Kind kind = Kind::started;  ///< Started or ended.
    Outage outage;              ///< The outage as known at that moment.
};

/**
 * @brief A state machine that turns rounds, in order, into outage events.
 *
 * Down rounds in a row become an outage once there are
 * OutageSettings::failures_to_start of them; the outage starts at the first
 * of them. Up rounds in a row (OutageSettings::successes_to_end) end it at
 * the first of them. Unknown rounds are ignored, but don't bridge a gap;
 * inconclusive rounds are ignored and do. Being a pure function of
 * the rounds, it serves the running service and the report, which can
 * rebuild outages from `connectivity.csv`.
 */
class OutageTracker {
public:
    /**
     * @brief A tracker with nothing seen yet.
     * @param settings Thresholds.
     * @throws std::invalid_argument if a threshold is below 1 or max_gap isn't positive.
     */
    explicit OutageTracker(OutageSettings settings = {});

    /**
     * @brief Feeds the next round.
     * @param round The round; rounds should come in time order.
     * @return The events it caused: none, one, or two (an outage ended by a
     *         gap, then a new one if the threshold is 1).
     */
    std::vector<OutageEvent> observe(const ConnectivityRound& round);

    /**
     * @brief Feeds the next round, already assessed (e.g. rebuilt from CSV rows).
     * @param time When the round started.
     * @param a    Its assessment.
     * @return As observe().
     */
    std::vector<OutageEvent> observe(Timestamp time, const RoundAssessment& a);

    /**
     * @brief Ends an open outage because observing stops (shutdown, end of
     *        the report range).
     * @return The `ended` event (OutageEnd::stopped), or nothing.
     */
    std::optional<OutageEvent> finish();

    /// @return The open outage, if any.
    [[nodiscard]] const std::optional<Outage>& current() const { return open_; }

private:
    /**
     * @brief Closes the open outage.
     * @param end    End time.
     * @param reason Why.
     * @return The `ended` event.
     */
    OutageEvent close(Timestamp end, OutageEnd reason);

    OutageSettings settings_;           ///< Thresholds.
    std::optional<Timestamp> last_;     ///< Time of the previous known (up or down) round.
    std::optional<Outage> open_;        ///< The outage in progress.
    Timestamp last_down_{};             ///< Last down round of open_.
    int downs_ = 0;                     ///< Down rounds in a row, before an outage opens.
    Outage pending_;                    ///< The outage being counted by downs_.
    int ups_ = 0;                       ///< Up rounds in a row while open_.
    Timestamp first_up_{};              ///< First of those.
};

}  // namespace meerkat::core
