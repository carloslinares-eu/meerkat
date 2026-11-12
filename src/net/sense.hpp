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

/// Where the connection details come from; replaceable in tests.
struct LinkSensors {
    std::function<std::optional<DefaultRoute>()> route;   ///< Finds the default route.
    std::function<LinkInfo(std::string_view)> link;       ///< Reads sysfs for an interface.
    std::function<WifiReading(std::string_view)> wifi;   ///< Reads Wi-Fi details (`iw`).
};

/**
 * @brief The real system: /proc/net/route, /sys/class/net and `iw`.
 * @return The sensors.
 */
LinkSensors system_link_sensors();

/**
 * @brief Reads the connection: the default route, its interface's link
 *        (type, carrier, speed) and, over an associated Wi-Fi, the Wi-Fi
 *        details.
 * @param sensors Where to read; `wifi` may be empty (no Wi-Fi details).
 * @return The snapshot.
 */
core::LinkSnapshot sense_link(const LinkSensors& sensors);

}  // namespace meerkat::net
