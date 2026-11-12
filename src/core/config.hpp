/**
 * @file config.hpp
 * @brief meerkat's settings: defaults, the TOML config file, and validation.
 *
 * Precedence, lowest first: built-in defaults → config file → command-line
 * flags (applied by the app layer). The contract figures have no defaults:
 * they are private and come only from the user's file or flags.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace meerkat::core {

/// The three speeds of one direction from the provider's product sheet, in Mbit/s.
struct ContractSpeeds {
    double maximum = 0;   ///< Maximum speed ("maximal").
    double normal = 0;    ///< Normally available speed ("normalerweise zur Verfügung stehend").
    double minimum = 0;   ///< Minimum speed ("minimal").

    /// @return True if the values are equal.
    bool operator==(const ContractSpeeds&) const = default;
};

/// The contract; each direction is empty when not configured.
struct Contract {
    std::optional<ContractSpeeds> download;  ///< Download speeds.
    std::optional<ContractSpeeds> upload;    ///< Upload speeds.
};

/// Everything meerkat can be configured with.
struct Config {
    std::filesystem::path data_dir;                         ///< Where CSVs, raw log and manifests go (absolute).
    std::chrono::seconds speed_interval{300};               ///< Time between speed tests.
    std::chrono::seconds connectivity_interval{30};         ///< Time between connectivity probes.
    std::string backend = "ookla";                          ///< Speed-test backend (only `ookla` so far).
    std::string speedtest = "speedtest";                    ///< Ookla CLI program name or path.
    std::optional<std::int64_t> server_id;                  ///< Pinned Ookla server; empty: Ookla picks.
    /// Public hosts to ping besides the gateway (Cloudflare and Quad9 by default).
    std::vector<std::string> ping_hosts{"1.1.1.1", "9.9.9.9"};
    bool ping_gateway = true;                               ///< Also ping the default gateway.
    std::string dns_host = "example.com";                   ///< Name whose lookup is timed.
    bool require_wired = false;                             ///< Skip speed tests over Wi-Fi (`skipped_wlan`).
    Contract contract;                                      ///< The contract figures, if given.
};

/// Error in a config file or a flag value; the message says where and why.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Looks up an environment variable; tests pass a fake.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/**
 * @brief Reads the real environment.
 * @param name Variable name.
 * @return Its value, or empty if it isn't set or is empty.
 */
std::optional<std::string> system_env(std::string_view name);

/**
 * @brief The built-in defaults, with the default data directory.
 *
 * Data directory: `$STATE_DIRECTORY` (set by systemd's `StateDirectory=`),
 * else `$XDG_DATA_HOME/meerkat`, else `$HOME/.local/share/meerkat`, else
 * `/var/lib/meerkat`.
 *
 * @param env Environment lookup.
 * @return The defaults.
 */
Config default_config(const EnvLookup& env = system_env);

/**
 * @brief Where meerkat looks for a config file when `--config` isn't given,
 *        in order: `$XDG_CONFIG_HOME/meerkat/config.toml` (or
 *        `$HOME/.config/meerkat/config.toml`), then `/etc/meerkat/config.toml`.
 * @param env Environment lookup.
 * @return The candidates; the first that exists is used.
 */
std::vector<std::filesystem::path> config_search_path(const EnvLookup& env = system_env);

/**
 * @brief Parses a duration: whole seconds, or a number with `s`, `m` or `h`
 *        (`30`, `30s`, `5m`, `1h`).
 * @param text The text.
 * @return The duration, or empty if the text isn't one (or is negative or huge).
 */
std::optional<std::chrono::seconds> parse_duration(std::string_view text);

/**
 * @brief Formats a duration the way parse_duration() reads it: `5m`, `30s`,
 *        `1h`, or seconds when it isn't a whole number of minutes.
 * @param d The duration.
 * @return The text.
 */
std::string format_duration(std::chrono::seconds d);

/**
 * @brief Parses contract speeds given as `MAX/NORMAL/MIN` in Mbit/s, e.g.
 *        `250/200/150`.
 * @param text The text.
 * @return The speeds; not yet validated (see validate()).
 * @throws ConfigError if the text isn't three numbers separated by `/`.
 */
ContractSpeeds parse_contract_speeds(std::string_view text);

/**
 * @brief Expands a leading `~/` with `$HOME`.
 * @param text The path text.
 * @param env  Environment lookup.
 * @return The path.
 * @throws ConfigError if it starts with `~` but `$HOME` isn't set, or uses
 *         the `~user` form.
 */
std::filesystem::path expand_home(std::string_view text, const EnvLookup& env = system_env);

/**
 * @brief Applies a TOML config file's text on top of @p base.
 *
 * Layout (every key optional; unknown keys are errors, so typos don't go
 * unnoticed):
 * @code
 * data_dir = "~/meerkat-data"     # absolute or ~/…
 * [speed]
 * interval = "5m"                 # or seconds as an integer
 * backend = "ookla"
 * program = "speedtest"
 * server_id = 12345
 * require_wired = false
 * [connectivity]
 * interval = "30s"
 * ping_hosts = ["1.1.1.1", "9.9.9.9"]
 * ping_gateway = true
 * dns_host = "example.com"
 * [contract]                      # Mbit/s, from the product sheet
 * download = { maximum = 250, normal = 200, minimum = 150 }
 * upload = { maximum = 50, normal = 40, minimum = 30 }
 * @endcode
 *
 * @param text   The file content.
 * @param source Name for error messages (usually the path).
 * @param base   The values to start from (normally default_config()).
 * @param env    Environment lookup, for `~` in `data_dir`.
 * @return The merged config; not yet validated.
 * @throws ConfigError with `source:line:column` for syntax errors, unknown
 *         keys and values of the wrong type.
 */
Config parse_config(std::string_view text, std::string_view source, Config base,
                    const EnvLookup& env = system_env);

/**
 * @brief Reads a config file and applies it on top of @p base.
 * @param path The file.
 * @param base The values to start from.
 * @param env  Environment lookup.
 * @return The merged config; not yet validated.
 * @throws ConfigError if the file can't be read or is invalid.
 */
Config load_config_file(const std::filesystem::path& path, Config base,
                        const EnvLookup& env = system_env);

/**
 * @brief Checks a complete config for values meerkat can't work with.
 *
 * - data_dir: absolute.
 * - speed interval 1 min – 24 h; connectivity interval 5 s – 1 h, and not
 *   longer than the speed interval.
 * - backend: `ookla`; program: not empty; server id: positive.
 * - ping hosts: at most 10, each a valid ping target; at least one host or
 *   the gateway. DNS host: not empty, no spaces.
 * - contract: every speed > 0 and at most 100 000 Mbit/s, and
 *   maximum ≥ normal ≥ minimum.
 *
 * @param c The config.
 * @return One message per problem; empty if the config is fine.
 */
std::vector<std::string> validate(const Config& c);

}  // namespace meerkat::core
