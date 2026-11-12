/**
 * @file settings.hpp
 * @brief Works out the effective config: defaults, then the config file,
 *        then the command-line flags.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/config.hpp"

namespace meerkat::app {

/// Config values given on the command line; empty means "not given".
struct Overrides {
    std::optional<std::filesystem::path> config_file;      ///< `--config`: this file, which must exist.
    std::optional<std::string> data_dir;                   ///< `--data-dir`.
    std::optional<std::string> speed_interval;             ///< `--speed-interval`, e.g. `5m`.
    std::optional<std::string> connectivity_interval;      ///< `--connectivity-interval`, e.g. `30s`.
    std::optional<std::string> speedtest;                  ///< `--speedtest`: program name or path.
    std::optional<std::int64_t> server_id;                 ///< `--server-id`.
    std::optional<std::vector<std::string>> ping_hosts;    ///< `--ping` (replaces the list).
    std::optional<std::string> dns_host;                   ///< `--dns-host`.
    std::optional<bool> require_wired;                     ///< `--require-wired` / `--no-require-wired`.
    std::optional<std::string> contract_down;              ///< `--contract-down MAX/NORMAL/MIN`.
    std::optional<std::string> contract_up;                ///< `--contract-up MAX/NORMAL/MIN`.
};

/// How the config was obtained, and what is wrong with it.
struct ConfigReport {
    std::optional<std::filesystem::path> file;       ///< The file read; empty if none.
    std::vector<std::filesystem::path> searched;     ///< Where it was looked for, if no `--config`.
    std::string load_error;                          ///< Why the file couldn't be used, if so.
    bool file_has_contract = false;                  ///< The file itself holds contract figures.
    /// Config files found while searching that this user can't read; they
    /// were skipped (e.g. a root-only `/etc/meerkat/config.toml`).
    std::vector<std::filesystem::path> unreadable;
    std::vector<std::string> problems;               ///< Bad flag values and validate() findings.

    /// @return True if the config can be used: it loaded and has no problems.
    [[nodiscard]] bool usable() const { return load_error.empty() && problems.empty(); }
};

/// The effective config and its report.
struct Settings {
    core::Config config;   ///< Defaults ← file ← flags (the file is skipped if it failed to load).
    ConfigReport report;   ///< Where it came from and what's wrong.
};

/**
 * @brief Builds the effective config. Never throws: every problem ends up in
 *        the report, so `meerkat check` can list them all.
 *
 * Without `--config`, the first readable file of core::config_search_path()
 * is read; none is fine (defaults). Files that exist but can't be read by
 * this user are skipped and listed in ConfigReport::unreadable. A file given
 * with `--config` must be readable. A relative `--data-dir` is taken
 * relative to the current directory.
 *
 * @param o   The flags.
 * @param env Environment lookup.
 * @return The settings.
 */
Settings resolve_settings(const Overrides& o, const core::EnvLookup& env = core::system_env);

}  // namespace meerkat::app
