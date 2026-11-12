/**
 * @file link_guard.hpp
 * @brief Decides before each speed test whether it may run, and warns when
 *        the connection is or becomes Wi-Fi.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/measurement.hpp"

namespace meerkat::core {

/// The connection as found right now (see net::sense_link()).
struct LinkSnapshot {
    bool has_route = false;          ///< A default route exists.
    bool interface_exists = false;   ///< Its interface is in /sys/class/net.
    std::optional<bool> carrier;     ///< Cable plugged in / Wi-Fi associated; empty if unknown.
    NetworkContext context;          ///< Interface, gateway, link type and speed, Wi-Fi details.
    std::string wifi_error;          ///< Why the Wi-Fi details are missing, if they are.

    /**
     * @brief Whether the connection can be used: a route, and a carrier
     *        unless the carrier is unknown.
     * @return True if online as far as the computer can tell.
     */
    [[nodiscard]] bool usable() const { return has_route && (!carrier || *carrier); }
};

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
 *   to Wi-Fi, online to offline, …), not on every test. Going back to a
 *   usable cable gives a non-warning notice.
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
