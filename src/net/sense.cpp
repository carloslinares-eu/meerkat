/**
 * @file sense.cpp
 * @brief Implementation of connection sensing.
 */
#include "net/sense.hpp"

#include <string>

namespace meerkat::net {

LinkSensors system_link_sensors() {
    LinkSensors s;
    s.route = [] { return default_route(); };
    s.link = [](std::string_view i) { return read_link(i); };
    s.wifi = [](std::string_view i) { return read_wifi(i); };
    return s;
}

core::LinkSnapshot sense_link(const LinkSensors& sensors) {
    core::LinkSnapshot s;
    const auto route = sensors.route();
    if (!route) {
        return s;
    }
    s.has_route = true;
    s.context.interface = route->interface;
    s.context.gateway = route->gateway;

    const auto link = sensors.link(route->interface);
    s.interface_exists = link.exists;
    s.carrier = link.carrier;
    s.context.link_type = link.type;
    s.context.link_speed_mbps = link.speed_mbps;

    if (link.exists && link.type == core::LinkType::wifi && sensors.wifi) {
        const auto wifi = sensors.wifi(route->interface);
        if (wifi.status == core::Status::ok) {
            s.context.wifi = wifi.info;
        } else {
            s.wifi_error = std::string{core::to_string(wifi.status)};
            if (!wifi.error.empty()) {
                s.wifi_error += ": " + wifi.error;
            }
        }
    }
    return s;
}

}  // namespace meerkat::net
