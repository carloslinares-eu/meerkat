/**
 * @file link_guard.cpp
 * @brief Implementation of the link guard.
 */
#include "core/link_guard.hpp"

namespace meerkat::core {

std::vector<std::string> link_warnings(const LinkSnapshot& s) {
    std::vector<std::string> w;
    if (!s.has_route) {
        w.emplace_back("no default route: this computer is offline.");
        return w;
    }
    const std::string& iface = s.context.interface;
    if (!s.interface_exists) {
        // The link type can't mean anything then.
        w.push_back("interface " + iface +
                    " of the default route isn't in /sys/class/net (did it just go away?).");
        return w;
    }
    if (!s.usable()) {
        w.push_back("no carrier on " + iface +
                    " (cable unplugged or Wi-Fi not connected): this computer is offline.");
    }
    if (s.context.link_type == LinkType::wifi) {
        w.push_back("the connection is Wi-Fi (" + iface +
                    "). Wi-Fi results don't count against the provider; connect a cable "
                    "to the router.");
    } else if (s.context.link_type == LinkType::unknown) {
        w.push_back("can't tell whether " + iface +
                    " is a cable (bridge, VPN or virtual interface?).");
    }
    return w;
}

LinkCheck LinkGuard::check(const LinkSnapshot& snapshot) {
    LinkCheck c;
    c.snapshot = snapshot;

    if (!snapshot.usable()) {
        SpeedResult s;
        s.status = Status::no_network;
        s.error = snapshot.has_route ? "not started: no carrier" : "not started: no default route";
        c.skip = std::move(s);
    } else if (require_wired_ && snapshot.context.link_type == LinkType::wifi) {
        SpeedResult s;
        s.status = Status::skipped_wlan;
        s.error = "not started: the connection is Wi-Fi and require_wired is set";
        c.skip = std::move(s);
    }

    const State now{snapshot.has_route, snapshot.interface_exists, snapshot.usable(),
                    snapshot.context.link_type};
    if (!last_ || *last_ != now) {
        for (auto& text : link_warnings(snapshot)) {
            c.notices.push_back({true, std::move(text)});
        }
        const bool good = now.usable && now.interface_exists && now.type == LinkType::wired;
        if (last_ && good) {
            c.notices.push_back(
                {false, "the connection is a cable again (" + snapshot.context.interface + ")."});
        }
    }
    last_ = now;
    return c;
}

}  // namespace meerkat::core
