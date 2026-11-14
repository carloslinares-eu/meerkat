/**
 * @file once.cpp
 * @brief Implementation of `meerkat once`.
 */
#include "app/once.hpp"

#include <algorithm>
#include <iomanip>
#include <memory>
#include <ostream>
#include <sstream>

#include "core/link_guard.hpp"
#include "core/status.hpp"
#include "core/text.hpp"
#include "net/sense.hpp"
#include "probe/dns.hpp"
#include "probe/ookla.hpp"
#include "probe/ping.hpp"

namespace meerkat::app {

namespace {

/// Shown for values that weren't measured.
constexpr std::string_view kMissing = "–";

/**
 * @brief Formats @p v with @p decimals decimals, always with `.`.
 * @param v        Value, or empty.
 * @param decimals Digits after the point.
 * @param unit     Appended after a space when there is a value.
 * @return The text, or `–`.
 */
std::string num(std::optional<double> v, int decimals, std::string_view unit = {}) {
    if (!v) {
        return std::string{kMissing};
    }
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::fixed << std::setprecision(decimals) << *v;
    if (!unit.empty()) {
        s << ' ' << unit;
    }
    return s.str();
}

/**
 * @brief Prints a `  label   value` row.
 * @param out   Stream.
 * @param label Left column.
 * @param value Right column.
 */
void row(std::ostream& out, std::string_view label, std::string_view value) {
    constexpr std::size_t kWidth = 14;
    std::string line{"  "};
    line += label;
    line.append(label.size() < kWidth ? kWidth - label.size() : 1, ' ');
    line += core::text::visible(value);  // SSIDs, hosts and tool errors come from outside
    line += '\n';
    out << line;  // no stream manipulators: the caller's flags stay unchanged
}

}  // namespace

Probes system_probes(const OnceOptions& options) {
    Probes p;
    p.sensors = net::system_link_sensors();
    p.ping = [](std::string_view t) { return probe::ping(t); };
    p.dns = [](std::string_view h) { return probe::resolve(h); };
    p.processes = [] { return sys::list_processes(); };
    if (options.speed) {
        auto backend = std::make_shared<probe::OoklaBackend>(
            probe::OoklaOptions{.program = options.speedtest, .server_id = options.server_id});
        p.speed = [backend](const sys::StopCheck& stop) { return backend->measure(stop); };
    }
    return p;
}

OnceOptions options_from(const core::Config& config) {
    OnceOptions o;
    o.server_id = config.server_id;
    o.speedtest = config.speedtest;
    o.ping_hosts = config.ping_hosts;
    o.ping_gateway = config.ping_gateway;
    o.dns_host = config.dns_host;
    o.require_wired = config.require_wired;
    return o;
}

OnceReport measure_once(const OnceOptions& options, const Probes& probes,
                        const std::function<void(std::string_view)>& progress) {
    const auto say = [&progress](std::string_view line) {
        if (progress) {
            progress(line);
        }
    };
    OnceReport r;
    r.time = probes.now();
    const auto add_warning = [&](std::string text) {
        say("Warning: " + text);
        r.warnings.push_back(std::move(text));
    };

    // The same guard `meerkat run` uses before every speed test; here it
    // runs once, so it reports everything that's wrong.
    core::LinkGuard guard{options.require_wired};
    const auto link = guard.check(net::sense_link(probes.sensors));
    r.has_route = link.snapshot.has_route;
    r.link_up = link.snapshot.usable();
    r.context = link.snapshot.context;
    r.carrier = link.snapshot.carrier;
    r.wifi_error = link.snapshot.wifi_error;
    for (const auto& n : link.notices) {
        if (n.warning) {
            add_warning(n.text);
        } else {
            say(n.text);
        }
    }

    say("Pinging…");
    if (options.ping_gateway && !r.context.gateway.empty()) {
        r.pings.push_back({.label = "gateway", .result = probes.ping(r.context.gateway)});
    }
    for (const auto& host : options.ping_hosts) {
        r.pings.push_back({.label = "public", .result = probes.ping(host)});
    }
    say("Resolving " + options.dns_host + "…");
    r.dns = probes.dns(options.dns_host);
    // Same labels as `meerkat run`: a failure while offline is no_network.
    for (auto& p : r.pings) {
        p.result.status = link.snapshot.probe_status(p.result.status);
    }
    r.dns.status = link.snapshot.probe_status(r.dns.status);

    if (options.speed && probes.speed) {
        if (link.skip) {
            r.speed = *link.skip;
        } else {
            say("Running the speed test (about 30 s)…");
            r.speed = probes.speed({});
        }
    }
    return r;
}

void print_report(const OnceReport& r, std::ostream& out) {
    out << "meerkat once  " << core::to_iso8601(r.time) << "\n\nConnection\n";
    if (!r.has_route) {
        row(out, "Route", "none (offline)");
    } else {
        std::string iface = r.context.interface + " (" +
                            std::string{core::to_string(r.context.link_type)};
        if (r.context.link_speed_mbps) {
            iface += ", link " + num(r.context.link_speed_mbps, 0, "Mbit/s");
        }
        if (r.carrier && !*r.carrier) {
            iface += ", no carrier";
        }
        row(out, "Interface", iface + ")");
        row(out, "Gateway", r.context.gateway.empty() ? std::string{kMissing} : r.context.gateway);
        if (r.context.wifi) {
            const auto& w = *r.context.wifi;
            row(out, "Wi-Fi", (w.ssid.empty() ? std::string{kMissing} : w.ssid) + ", signal " +
                                  num(w.signal_dbm, 0, "dBm") + ", " +
                                  num(w.frequency_mhz, 0, "MHz") + ", rx " +
                                  num(w.rx_bitrate_mbps, 1, "Mbit/s") + ", tx " +
                                  num(w.tx_bitrate_mbps, 1, "Mbit/s"));
        } else if (!r.wifi_error.empty()) {
            row(out, "Wi-Fi", r.wifi_error);
        }
    }

    out << "\nConnectivity\n";
    for (const auto& [label, p] : r.pings) {
        std::string line;
        if (p.status == core::Status::ok) {
            line = "loss " + num(p.loss_percent(), 1, "%") + ", rtt min/avg/max " +
                   num(p.rtt_min_ms, 1) + " / " + num(p.rtt_avg_ms, 1) + " / " +
                   num(p.rtt_max_ms, 1, "ms") + ", jitter " + num(p.rtt_mdev_ms, 1, "ms");
            if (!p.error.empty()) {
                line += " (" + p.error + ")";
            }
        } else {
            line = core::status_text(p.status, p.error);
        }
        row(out, "Ping " + label, p.target + "  " + line);
    }
    row(out, "DNS", r.dns.host + "  " +
                        (r.dns.status == core::Status::ok ? num(r.dns.duration_ms, 1, "ms")
                                                          : core::status_text(r.dns.status, r.dns.error)));

    if (r.speed) {
        const auto& s = *r.speed;
        out << "\nSpeed test ("
            << core::text::visible(s.backend.empty() ? core::kOoklaBackend : s.backend) << ")\n";
        if (s.status != core::Status::ok || !s.error.empty()) {
            row(out, "Status", core::status_text(s.status, s.error));
        }
        if (s.status == core::Status::ok || s.download_mbps || s.upload_mbps) {
            row(out, "Download", num(s.download_mbps, 2, "Mbit/s"));
            row(out, "Upload", num(s.upload_mbps, 2, "Mbit/s"));
            row(out, "Latency", num(s.latency_ms, 1, "ms") + " (jitter " + num(s.jitter_ms, 1, "ms") + ")");
            row(out, "Packet loss", num(s.packet_loss_percent, 1, "%"));
            std::string server = s.server_name.empty() ? std::string{kMissing} : s.server_name;
            if (!s.server_location.empty()) {
                server += ", " + s.server_location;
            }
            if (s.server_id) {
                server += " (id " + std::to_string(*s.server_id) + ")";
            }
            row(out, "Server", server);
            row(out, "ISP", s.isp.empty() ? std::string{kMissing} : s.isp);
            row(out, "External IP", s.external_ip.empty() ? std::string{kMissing} : s.external_ip);
            row(out, "Result", s.result_url.empty() ? std::string{kMissing} : s.result_url);
        }
    }

    if (!r.warnings.empty()) {
        out << '\n';
        for (const auto& w : r.warnings) {
            out << "Warning: " << core::text::visible(w) << '\n';
        }
    }
}

int run_once(const OnceOptions& options, std::ostream& out, std::ostream& err) {
    const auto report = measure_once(options, system_probes(options),
                                     [&err](std::string_view line) {
                                         err << core::text::visible(line) << '\n' << std::flush;
                                     });
    print_report(report, out);
    const bool any_public_reply = std::ranges::any_of(report.pings, [](const PingEntry& p) {
        return p.label == "public" && p.result.received.value_or(0) > 0;
    });
    const bool no_public_hosts =
        std::ranges::none_of(report.pings, [](const PingEntry& p) { return p.label == "public"; });
    const bool online = report.has_route && report.link_up && (any_public_reply || no_public_hosts);
    return online ? 0 : 1;
}

}  // namespace meerkat::app
