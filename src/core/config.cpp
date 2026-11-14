/**
 * @file config.cpp
 * @brief Implementation of the config file and its validation.
 */
#include "core/config.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <sstream>

#include "core/text.hpp"
#include "probe/ping.hpp"

namespace meerkat::core {

namespace {

/// Largest contract speed accepted, in Mbit/s (100 Gbit/s).
constexpr double kMaxMbps = 100'000;

/**
 * @brief Builds a ConfigError pointing at a place in the file.
 * @param source File name.
 * @param where  Source region of the offending node or key.
 * @param what   The problem.
 * @return The error.
 */
ConfigError error_at(std::string_view source, const toml::source_region& where,
                     const std::string& what) {
    std::ostringstream s;
    s << source << ':' << where.begin.line << ':' << where.begin.column << ": " << what;
    return ConfigError{s.str()};
}

/// One TOML table being read: rejects unknown keys, typed getters.
class Section {
public:
    /**
     * @brief Wraps @p table.
     * @param table  The table.
     * @param name   Its name for messages (`""` for the top level).
     * @param source File name for messages.
     */
    Section(const toml::table& table, std::string name, std::string_view source)
        : table_{table}, name_{std::move(name)}, source_{source} {}

    /**
     * @brief Throws for keys not in @p allowed.
     * @param allowed The known keys.
     */
    void only(std::initializer_list<std::string_view> allowed) const {
        for (auto&& [key, node] : table_) {
            bool known = false;
            for (const auto a : allowed) {
                known = known || key.str() == a;
            }
            if (!known) {
                throw error_at(source_, key.source(), "unknown setting '" + qualified(key.str()) + "'");
            }
        }
    }

    /**
     * @brief A string value.
     * @param key The key.
     * @return The value, or empty if the key is absent.
     * @throws ConfigError if it has another type.
     */
    std::optional<std::string> string(std::string_view key) const {
        const auto* n = table_.get(key);
        if (n == nullptr) {
            return std::nullopt;
        }
        if (!n->is_string()) {
            throw type_error(*n, key, "a string");
        }
        return n->value<std::string>();
    }

    /**
     * @brief A boolean value.
     * @param key The key.
     * @return The value, or empty if absent.
     * @throws ConfigError if it has another type.
     */
    std::optional<bool> boolean(std::string_view key) const {
        const auto* n = table_.get(key);
        if (n == nullptr) {
            return std::nullopt;
        }
        if (!n->is_boolean()) {
            throw type_error(*n, key, "true or false");
        }
        return n->value<bool>();
    }

    /**
     * @brief An integer value.
     * @param key The key.
     * @return The value, or empty if absent.
     * @throws ConfigError if it has another type.
     */
    std::optional<std::int64_t> integer(std::string_view key) const {
        const auto* n = table_.get(key);
        if (n == nullptr) {
            return std::nullopt;
        }
        if (!n->is_integer()) {
            throw type_error(*n, key, "a whole number");
        }
        return n->value<std::int64_t>();
    }

    /**
     * @brief A duration: an integer (seconds) or a string like `5m`.
     * @param key The key.
     * @return The value, or empty if absent.
     * @throws ConfigError if it isn't a duration.
     */
    std::optional<std::chrono::seconds> duration(std::string_view key) const {
        const auto* n = table_.get(key);
        if (n == nullptr) {
            return std::nullopt;
        }
        std::optional<std::chrono::seconds> d;
        if (n->is_integer()) {
            d = parse_duration(std::to_string(n->value<std::int64_t>().value_or(-1)));
        } else if (n->is_string()) {
            d = parse_duration(n->value<std::string>().value_or(""));
        }
        if (!d) {
            throw type_error(*n, key, "a duration such as 30, \"30s\", \"5m\" or \"1h\"");
        }
        return d;
    }

    /**
     * @brief An array of strings.
     * @param key The key.
     * @return The values, or empty if absent.
     * @throws ConfigError if it isn't an array of strings.
     */
    std::optional<std::vector<std::string>> strings(std::string_view key) const {
        const auto* n = table_.get(key);
        if (n == nullptr) {
            return std::nullopt;
        }
        const auto* arr = n->as_array();
        if (arr == nullptr) {
            throw type_error(*n, key, "a list of strings");
        }
        std::vector<std::string> out;
        for (const auto& item : *arr) {
            if (!item.is_string()) {
                throw type_error(item, key, "a list of strings");
            }
            out.push_back(item.value<std::string>().value_or(""));
        }
        return out;
    }

    /**
     * @brief A sub-table.
     * @param key The key.
     * @return The table, or null if absent.
     * @throws ConfigError if it isn't a table.
     */
    const toml::table* table(std::string_view key) const {
        const auto* n = table_.get(key);
        if (n == nullptr) {
            return nullptr;
        }
        if (!n->is_table()) {
            throw type_error(*n, key, "a table");
        }
        return n->as_table();
    }

    /**
     * @brief A number (integer or float) at @p key.
     * @param key The key.
     * @return The value, or empty if absent.
     * @throws ConfigError if it isn't a number.
     */
    std::optional<double> number(std::string_view key) const {
        const auto* n = table_.get(key);
        if (n == nullptr) {
            return std::nullopt;
        }
        if (!n->is_number()) {
            throw type_error(*n, key, "a number");
        }
        return n->value<double>();
    }

    /**
     * @brief `section.key`, or `key` at the top level.
     * @param key The key.
     * @return The qualified name.
     */
    std::string qualified(std::string_view key) const {
        return name_.empty() ? std::string{key} : name_ + "." + std::string{key};
    }

    /// @return The source region of the table itself.
    const toml::source_region& where() const { return table_.source(); }

private:
    /**
     * @brief The error for a value of the wrong type.
     * @param n        The node.
     * @param key      Its key.
     * @param expected What was expected.
     * @return The error.
     */
    ConfigError type_error(const toml::node& n, std::string_view key,
                           std::string_view expected) const {
        return error_at(source_, n.source(),
                        "'" + qualified(key) + "' must be " + std::string{expected});
    }

    const toml::table& table_;  ///< The table.
    std::string name_;          ///< Its name.
    std::string_view source_;   ///< File name.
};

/**
 * @brief Reads one contract direction: `{ maximum = …, normal = …, minimum = … }`.
 * @param parent The `[contract]` section.
 * @param key    `download` or `upload`.
 * @param source File name.
 * @return The speeds, or empty if the key is absent.
 * @throws ConfigError if a value is missing or not a number.
 */
std::optional<ContractSpeeds> read_speeds(const Section& parent, std::string_view key,
                                          std::string_view source) {
    const auto* t = parent.table(key);
    if (t == nullptr) {
        return std::nullopt;
    }
    const Section s{*t, parent.qualified(key), source};
    s.only({"maximum", "normal", "minimum"});
    ContractSpeeds c;
    const auto get = [&](std::string_view k) {
        const auto v = s.number(k);
        if (!v) {
            throw error_at(source, s.where(), "'" + s.qualified(k) + "' is missing");
        }
        return *v;
    };
    c.maximum = get("maximum");
    c.normal = get("normal");
    c.minimum = get("minimum");
    return c;
}

}  // namespace

std::optional<std::string> system_env(std::string_view name) {
    const char* v = std::getenv(std::string{name}.c_str());
    if (v == nullptr || *v == '\0') {
        return std::nullopt;
    }
    return std::string{v};
}

Config default_config(const EnvLookup& env) {
    Config c;
    if (const auto state = env("STATE_DIRECTORY"); state && !state->empty() && state->front() == '/') {
        // systemd may list several directories separated by ':'; ours is the first.
        c.data_dir = state->substr(0, state->find(':'));
    } else if (const auto xdg = env("XDG_DATA_HOME"); xdg && !xdg->empty() && xdg->front() == '/') {
        c.data_dir = std::filesystem::path{*xdg} / "meerkat";
    } else if (const auto home = env("HOME"); home && !home->empty()) {
        c.data_dir = std::filesystem::path{*home} / ".local" / "share" / "meerkat";
    } else {
        c.data_dir = "/var/lib/meerkat";
    }
    return c;
}

std::vector<std::filesystem::path> config_search_path(const EnvLookup& env) {
    std::vector<std::filesystem::path> paths;
    if (const auto xdg = env("XDG_CONFIG_HOME"); xdg && !xdg->empty() && xdg->front() == '/') {
        paths.push_back(std::filesystem::path{*xdg} / "meerkat" / "config.toml");
    } else if (const auto home = env("HOME"); home && !home->empty()) {
        paths.push_back(std::filesystem::path{*home} / ".config" / "meerkat" / "config.toml");
    }
    paths.emplace_back("/etc/meerkat/config.toml");
    return paths;
}

std::optional<std::chrono::seconds> parse_duration(std::string_view text) {
    std::int64_t factor = 1;
    if (!text.empty()) {
        switch (text.back()) {
        case 's': factor = 1; text.remove_suffix(1); break;
        case 'm': factor = 60; text.remove_suffix(1); break;
        case 'h': factor = 3600; text.remove_suffix(1); break;
        default: break;
        }
    }
    const auto n = text::parse_int<std::int64_t>(text);
    // Up to a week: anything longer is a typo, and the product can't overflow.
    if (!n || *n < 0 || *n > 7 * 24 * 3600 / factor) {
        return std::nullopt;
    }
    return std::chrono::seconds{*n * factor};
}

std::string format_duration(std::chrono::seconds d) {
    const auto s = d.count();
    if (s != 0 && s % 3600 == 0) {
        return std::to_string(s / 3600) + "h";
    }
    if (s != 0 && s % 60 == 0) {
        return std::to_string(s / 60) + "m";
    }
    return std::to_string(s) + "s";
}

ContractSpeeds parse_contract_speeds(std::string_view text) {
    std::vector<double> v;
    std::string_view rest = text;
    while (true) {
        const auto slash = rest.find('/');
        const auto d = text::parse_double(text::trim(rest.substr(0, slash)));
        if (!d) {
            v.clear();
            break;
        }
        v.push_back(*d);
        if (slash == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(slash + 1);
    }
    if (v.size() != 3) {
        throw ConfigError("contract speeds must be MAX/NORMAL/MIN in Mbit/s, e.g. 250/200/150; got '" +
                          std::string{text} + "'");
    }
    return {v[0], v[1], v[2]};
}

std::filesystem::path expand_home(std::string_view text, const EnvLookup& env) {
    if (text.empty() || text.front() != '~') {
        return std::filesystem::path{text};
    }
    if (text != "~" && !text.starts_with("~/")) {
        throw ConfigError("'" + std::string{text} + "': only ~/ (your own home) is supported");
    }
    const auto home = env("HOME");
    if (!home || home->empty()) {
        throw ConfigError("'" + std::string{text} + "' uses ~, but $HOME isn't set");
    }
    return std::filesystem::path{*home} / std::string{text.substr(text == "~" ? 1 : 2)};
}

Config parse_config(std::string_view text, std::string_view source, Config c,
                    const EnvLookup& env) {
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        throw error_at(source, e.source(), std::string{e.description()});
    }
    const Section top{root, "", source};
    top.only({"data_dir", "speed", "connectivity", "contract"});
    if (const auto d = top.string("data_dir")) {
        try {
            c.data_dir = expand_home(*d, env);
        } catch (const ConfigError& e) {
            throw error_at(source, root.get("data_dir")->source(), e.what());
        }
        if (!c.data_dir.is_absolute()) {
            throw error_at(source, root.get("data_dir")->source(),
                           "'data_dir' must be an absolute path or start with ~/");
        }
    }

    if (const auto* t = top.table("speed")) {
        const Section s{*t, "speed", source};
        s.only({"interval", "backend", "program", "server_id", "require_wired"});
        c.speed_interval = s.duration("interval").value_or(c.speed_interval);
        c.backend = s.string("backend").value_or(c.backend);
        c.speedtest = s.string("program").value_or(c.speedtest);
        if (const auto id = s.integer("server_id")) {
            c.server_id = *id;
        }
        c.require_wired = s.boolean("require_wired").value_or(c.require_wired);
    }
    if (const auto* t = top.table("connectivity")) {
        const Section s{*t, "connectivity", source};
        s.only({"interval", "ping_hosts", "ping_gateway", "dns_host", "outage_after"});
        c.connectivity_interval = s.duration("interval").value_or(c.connectivity_interval);
        c.ping_hosts = s.strings("ping_hosts").value_or(c.ping_hosts);
        c.ping_gateway = s.boolean("ping_gateway").value_or(c.ping_gateway);
        c.dns_host = s.string("dns_host").value_or(c.dns_host);
        if (const auto n = s.integer("outage_after")) {
            // Clamped to int; validate() reports anything outside 1-20.
            c.outage_after = static_cast<int>(std::clamp<std::int64_t>(
                *n, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
        }
    }
    if (const auto* t = top.table("contract")) {
        const Section s{*t, "contract", source};
        s.only({"download", "upload"});
        if (auto d = read_speeds(s, "download", source)) {
            c.contract.download = d;
        }
        if (auto u = read_speeds(s, "upload", source)) {
            c.contract.upload = u;
        }
    }
    return c;
}

Config load_config_file(const std::filesystem::path& path, Config base, const EnvLookup& env) {
    const auto text = text::read_file(path);
    if (!text) {
        throw ConfigError("can't read config file " + path.string());
    }
    return parse_config(*text, path.string(), std::move(base), env);
}

std::vector<std::string> validate(const Config& c) {
    using std::chrono::hours;
    using std::chrono::minutes;
    using std::chrono::seconds;
    std::vector<std::string> errors;
    if (!c.data_dir.is_absolute()) {
        errors.push_back("data_dir must be an absolute path (got '" + c.data_dir.string() + "')");
    }
    if (c.speed_interval < minutes{1} || c.speed_interval > hours{24}) {
        errors.push_back("the speed-test interval must be between 1m and 24h (got " +
                         format_duration(c.speed_interval) + ")");
    }
    if (c.connectivity_interval < seconds{5} || c.connectivity_interval > hours{1}) {
        errors.push_back("the connectivity interval must be between 5s and 1h (got " +
                         format_duration(c.connectivity_interval) + ")");
    } else if (c.connectivity_interval > c.speed_interval) {
        errors.push_back("the connectivity interval (" + format_duration(c.connectivity_interval) +
                         ") must not be longer than the speed-test interval (" +
                         format_duration(c.speed_interval) + ")");
    }
    if (c.backend != kOoklaBackend) {
        errors.push_back("unknown speed-test backend '" + c.backend + "' (supported: " +
                         std::string{kOoklaBackend} + ")");
    }
    if (c.speedtest.empty()) {
        errors.push_back("the speed-test program must not be empty");
    }
    if (c.server_id && *c.server_id <= 0) {
        errors.push_back("the server id must be a positive number");
    }
    if (c.ping_hosts.size() > 10) {
        errors.push_back("at most 10 ping hosts are allowed");
    }
    for (const auto& h : c.ping_hosts) {
        if (!probe::is_valid_ping_target(h)) {
            errors.push_back("invalid ping host '" + h + "'");
        }
    }
    if (c.ping_hosts.empty() && !c.ping_gateway) {
        errors.push_back("nothing to ping: give a ping host or enable ping_gateway");
    }
    if (c.dns_host.empty() || c.dns_host.find_first_of(" \t\r\n") != std::string::npos ||
        c.dns_host.size() > 253) {
        errors.push_back("invalid DNS host '" + c.dns_host + "'");
    }
    if (c.outage_after < 1 || c.outage_after > 20) {
        errors.push_back("outage_after must be between 1 and 20 rounds (got " +
                         std::to_string(c.outage_after) + ")");
    }
    const auto check_speeds = [&errors](const std::optional<ContractSpeeds>& s,
                                        std::string_view dir) {
        if (!s) {
            return;
        }
        const std::string d{dir};
        for (const double v : {s->maximum, s->normal, s->minimum}) {
            if (!std::isfinite(v) || v <= 0 || v > kMaxMbps) {
                errors.push_back("contract " + d + " speeds must be between 0 and 100000 Mbit/s (got " +
                                 text::to_text(v) + ")");
                return;
            }
        }
        if (!(s->maximum >= s->normal && s->normal >= s->minimum)) {
            errors.push_back("contract " + d + " speeds must be maximum ≥ normal ≥ minimum (got " +
                             text::to_text(s->maximum) + "/" + text::to_text(s->normal) + "/" + text::to_text(s->minimum) + ")");
        }
    };
    check_speeds(c.contract.download, "download");
    check_speeds(c.contract.upload, "upload");
    return errors;
}

}  // namespace meerkat::core
