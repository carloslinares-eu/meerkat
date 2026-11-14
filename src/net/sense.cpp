/**
 * @file sense.cpp
 * @brief Implementation of connection sensing.
 */
#include "net/sense.hpp"

#include "net/kernel_route.hpp"

#include <string>

namespace meerkat::net {

LinkSensors system_link_sensors() {
    LinkSensors s;
    s.route = [] { return system_route(); };
    s.link = [](std::string_view i) { return read_link(i); };
    s.wifi = [](std::string_view i) { return read_wifi(i); };
    return s;
}

core::LinkSnapshot sense_link(const LinkSensors& sensors) {
    core::LinkSnapshot s;
    const auto read_route = [&] { return sensors.route ? sensors.route() : std::nullopt; };
    auto route = read_route();
    if (!route || !sensors.link) {
        s.has_route = route.has_value();
        if (route) {
            s.context.interface = route->interface;
            s.context.gateway = route->gateway;
        }
        return s;  // no route, or nothing known about its interface: offline
    }
    auto link = sensors.link(route->interface);
    if (!link.exists) {
        // Renamed or re-plugged between reading the route and sysfs? Look
        // once more before calling the interface gone.
        if (auto again = read_route(); again && again->interface != route->interface) {
            route = std::move(again);
            link = sensors.link(route->interface);
        }
    }
    s.has_route = true;
    s.context.interface = route->interface;
    s.context.gateway = route->gateway;
    s.interface_exists = link.exists;
    s.carrier = link.carrier;
    s.context.link_type = link.type;
    s.context.link_speed_mbps = link.speed_mbps;

    // `iw` can only fail while Wi-Fi isn't associated: don't run it then.
    const bool associated = !link.carrier || *link.carrier;
    if (link.exists && link.type == core::LinkType::wifi && associated && sensors.wifi) {
        const auto wifi = sensors.wifi(route->interface);
        if (wifi.status == core::Status::ok) {
            s.context.wifi = wifi.info;
        } else {
            s.wifi_error = core::status_text(wifi.status, wifi.error);
            if (wifi.status == core::Status::no_network && !link.carrier) {
                // Not associated, or the radio is down. Only decides when
                // sysfs couldn't tell: a readable carrier wins (iw may run
                // mid-roam).
                s.carrier = false;
            }
        }
    }
    return s;
}

}  // namespace meerkat::net
