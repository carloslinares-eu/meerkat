/**
 * @file link_snapshot.hpp
 * @brief How the computer is connected at one moment.
 */
#pragma once

#include <optional>
#include <string>

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
     * @brief Whether the connection can be used: a route through an
     *        interface that exists, and a carrier unless the carrier is
     *        unknown.
     * @return True if online as far as the computer can tell.
     */
    [[nodiscard]] bool usable() const {
        return has_route && interface_exists && (!carrier || *carrier);
    }

    /**
     * @brief The status to record for a probe that ran on this connection.
     *
     * A probe that failed with `error` while the computer had no usable
     * connection (e.g. DNS: "Temporary failure in name resolution" with the
     * cable pulled) failed because of that: it becomes `no_network`, the
     * same as a ping that found no route. With a usable connection the
     * same error (e.g. the router answering SERVFAIL) stays `error`.
     * @param s The probe's own status.
     * @return @p s, or `no_network` as described.
     */
    [[nodiscard]] Status probe_status(Status s) const {
        return s == Status::error && !usable() ? Status::no_network : s;
    }
};

}  // namespace meerkat::core
