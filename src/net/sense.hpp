/**
 * @file sense.hpp
 * @brief Finds out how the computer is connected right now: default route,
 *        link type and carrier, Wi-Fi details.
 */
#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "core/link_snapshot.hpp"
#include "net/link.hpp"
#include "net/route.hpp"
#include "net/wifi.hpp"

namespace meerkat::net {

/// Where sense_link() gets the connection details; replaceable in tests.
/// An empty `route` reads as no route, an empty `link` as an interface not
/// known to exist (both offline), an empty `wifi` as no Wi-Fi details.
struct LinkSensors {
    std::function<std::optional<DefaultRoute>()> route;   ///< Finds the default route.
    std::function<LinkInfo(std::string_view)> link;       ///< Reads sysfs for an interface.
    std::function<WifiReading(std::string_view)> wifi;   ///< Reads Wi-Fi details (`iw`).
};

/**
 * @brief The real system: the kernel's route (system_route()),
 *        /sys/class/net and `iw`.
 * @return The sensors.
 */
LinkSensors system_link_sensors();

/**
 * @brief Reads the connection: the default route, its interface's link
 *        (type, carrier, speed) and, over an associated Wi-Fi, the Wi-Fi
 *        details.
 * If the route's interface isn't in sysfs, the route is read once more: an
 * interface renamed (udev) or re-plugged between the two reads then counts
 * under its new name instead of as gone. Over Wi-Fi with an unreadable
 * carrier, `iw` saying "not connected" (or the radio down) sets the carrier
 * to false.
 * @param sensors Where to read; any of them may be empty (see LinkSensors).
 * @return The snapshot.
 */
core::LinkSnapshot sense_link(const LinkSensors& sensors);

}  // namespace meerkat::net
