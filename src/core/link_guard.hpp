/**
 * @file link_guard.hpp
 * @brief Decides before each speed test whether it may run, and warns when
 *        the connection is or becomes Wi-Fi.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/link_snapshot.hpp"
#include "core/measurement.hpp"

namespace meerkat::core {

/// Something the guard has to say.
struct LinkNotice {
    bool warning = true;  ///< A warning; false for good news (back on a cable).
    std::string text;     ///< The message, a full sentence.
};

/// The guard's answer for one speed test.
struct LinkCheck {
    LinkSnapshot snapshot;                ///< What it decided on.
    /// Set if the speed test must not run: the row to record instead
    /// (status `no_network` or `skipped_wlan`, with the reason in `error`).
    std::optional<SpeedResult> skip;
    /// New notices: everything wrong on the first check, then only what
    /// changed (the same Wi-Fi warning isn't repeated every 5 minutes).
    std::vector<LinkNotice> notices;
};

/**
 * @brief Checks the connection before each speed test (the WLAN warning).
 *
 * - Over Wi-Fi it warns, because the provider will blame Wi-Fi for slow
 *   results; with `require_wired` the test is skipped (`skipped_wlan`).
 * - Offline (no default route, or no carrier) the test is skipped
 *   (`no_network`): a test that can't reach anything would only measure
 *   the outage, which the connectivity probe already records.
 * - Warnings come on the first check and whenever the state changes (cable
 *   to Wi-Fi, online to offline, another interface, …), not on every test.
 *   Coming back online, and moving from Wi-Fi or an unknown link to a cable,
 *   give a non-warning notice. Moving between two cables says nothing.
 */
class LinkGuard {
public:
    /**
     * @brief A guard that hasn't seen the connection yet.
     * @param require_wired Skip speed tests over Wi-Fi.
     */
    explicit LinkGuard(bool require_wired) : require_wired_{require_wired} {}

    /**
     * @brief Decides about one speed test.
     * @param snapshot The connection now.
     * @return The decision and any new notices.
     */
    LinkCheck check(const LinkSnapshot& snapshot);

private:
    /// What the warnings depend on; a change triggers new notices.
    struct State {
        bool has_route = false;                ///< LinkSnapshot::has_route.
        bool interface_exists = false;         ///< LinkSnapshot::interface_exists.
        bool usable = false;                   ///< LinkSnapshot::usable().
        LinkType type = LinkType::unknown;     ///< Link type.
        std::string interface;                 ///< Interface of the default route.
        /**
         * @brief Field-wise comparison.
         * @return True if every field is equal.
         */
        bool operator==(const State&) const = default;
    };

    bool require_wired_;            ///< Skip speed tests over Wi-Fi.
    std::optional<State> last_;     ///< State at the previous check.
};

/**
 * @brief The warnings that describe a connection, e.g. "the connection is Wi-Fi".
 * @param s The connection.
 * @return The warnings, in order of importance; empty for a usable cable.
 */
std::vector<std::string> link_warnings(const LinkSnapshot& s);

}  // namespace meerkat::core
