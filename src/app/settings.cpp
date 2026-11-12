/**
 * @file settings.cpp
 * @brief Implementation of the config resolution.
 */
#include "app/settings.hpp"

#include <system_error>

namespace meerkat::app {

Settings resolve_settings(const Overrides& o, const core::EnvLookup& env) {
    namespace fs = std::filesystem;
    Settings s;
    s.config = core::default_config(env);
    auto& r = s.report;

    if (o.config_file) {
        r.file = *o.config_file;
    } else {
        r.searched = core::config_search_path(env);
        for (const auto& p : r.searched) {
            std::error_code ec;
            if (fs::exists(p, ec)) {
                r.file = p;
                break;
            }
        }
    }
    if (r.file) {
        try {
            s.config = core::load_config_file(*r.file, s.config, env);
        } catch (const core::ConfigError& e) {
            r.load_error = e.what();
        }
    }

    auto& c = s.config;
    const auto flag_problem = [&r](const std::string& text) { r.problems.push_back(text); };
    if (o.data_dir) {
        try {
            c.data_dir = fs::absolute(core::expand_home(*o.data_dir, env));
        } catch (const std::exception& e) {
            flag_problem(std::string{"--data-dir: "} + e.what());
        }
    }
    const auto duration = [&](const std::optional<std::string>& text, const char* flag,
                              std::chrono::seconds& target) {
        if (!text) {
            return;
        }
        if (const auto d = core::parse_duration(*text)) {
            target = *d;
        } else {
            flag_problem(std::string{flag} + ": '" + *text +
                         "' isn't a duration such as 30s, 5m or 1h");
        }
    };
    duration(o.speed_interval, "--speed-interval", c.speed_interval);
    duration(o.connectivity_interval, "--connectivity-interval", c.connectivity_interval);
    if (o.speedtest) {
        c.speedtest = *o.speedtest;
    }
    if (o.server_id) {
        c.server_id = *o.server_id;
    }
    if (o.ping_hosts) {
        c.ping_hosts = *o.ping_hosts;
    }
    if (o.dns_host) {
        c.dns_host = *o.dns_host;
    }
    if (o.require_wired) {
        c.require_wired = true;
    }
    const auto contract = [&](const std::optional<std::string>& text, const char* flag,
                              std::optional<core::ContractSpeeds>& target) {
        if (!text) {
            return;
        }
        try {
            target = core::parse_contract_speeds(*text);
        } catch (const core::ConfigError& e) {
            flag_problem(std::string{flag} + ": " + e.what());
        }
    };
    contract(o.contract_down, "--contract-down", c.contract.download);
    contract(o.contract_up, "--contract-up", c.contract.upload);

    for (auto& p : core::validate(c)) {
        r.problems.push_back(std::move(p));
    }
    return s;
}

}  // namespace meerkat::app
