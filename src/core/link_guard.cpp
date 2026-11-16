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
    } else if (s.context.link_type == LinkType::tunnel) {
        w.push_back("the connection goes through a VPN or tunnel (" + iface +
                    "). Its results measure the VPN as much as the line and don't count "
                    "against the provider; turn the VPN off while measuring.");
    } else if (s.context.link_type == LinkType::unknown) {
        w.push_back("can't tell whether " + iface +
                    " is a cable (bridge, modem or virtual interface?).");
    }
    return w;
}

LinkCheck LinkGuard::check(const LinkSnapshot& snapshot) {
    LinkCheck c;
    c.snapshot = snapshot;

    if (!snapshot.usable()) {
        SpeedResult s;
        s.status = Status::no_network;
        s.error = not_started(!snapshot.has_route          ? "no default route"
                              : !snapshot.interface_exists ? "interface " + snapshot.context.interface + " is gone"
                                                           : "no carrier");
        c.skip = std::move(s);
    } else if (require_wired_ && snapshot.context.link_type == LinkType::wifi) {
        SpeedResult s;
        s.status = Status::skipped_wlan;
        s.error = not_started("the connection is Wi-Fi and require_wired is set");
        c.skip = std::move(s);
    } else if (require_wired_ && snapshot.context.link_type == LinkType::tunnel) {
        SpeedResult s;
        s.status = Status::skipped_tunnel;
        s.error = not_started("the connection goes through a VPN or tunnel and require_wired is set");
        c.skip = std::move(s);
    }

    const State now{snapshot.has_route, snapshot.interface_exists, snapshot.usable(),
                    snapshot.context.link_type, snapshot.context.interface};
    const auto good_cable = [](const State& st) { return st.usable && st.type == LinkType::wired; };
    if (last_ && *last_ != now) {
        const std::string& iface = snapshot.context.interface;
        if (!last_->usable && now.usable) {
            c.notices.push_back({false, "online again (" + iface + ", " +
                                            std::string{to_string(now.type)} + ")."});
        } else if (last_->usable && last_->type != LinkType::wired && good_cable(now)) {
            c.notices.push_back({false, "the connection is a cable now (" + iface + ")."});
        }
    }
    if (!last_ || *last_ != now) {
        for (auto& text : link_warnings(snapshot)) {
            c.notices.push_back({true, std::move(text)});
        }
    }
    last_ = now;
    return c;
}

}  // namespace meerkat::core
