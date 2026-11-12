/**
 * @file check.hpp
 * @brief `meerkat check`: is everything in place to record evidence?
 */
#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/settings.hpp"
#include "core/config.hpp"
#include "net/link.hpp"
#include "net/route.hpp"
#include "sys/process.hpp"

namespace meerkat::app {

/**
 * @brief What `meerkat check` looks at. Each part is replaceable so the
 *        tests run without the real system.
 */
struct CheckEnv {
    /// Finds a program on `$PATH` (and the sbin directories).
    std::function<std::optional<std::filesystem::path>(std::string_view)> find_program;
    sys::Runner run;                                                ///< Runs a program.
    std::function<std::optional<net::DefaultRoute>()> route;        ///< Finds the default route.
    std::function<net::LinkInfo(std::string_view)> link;            ///< Reads sysfs for an interface.
};

/**
 * @brief The real system: `$PATH`, sys::run, /proc and /sys.
 * @return The environment.
 */
CheckEnv system_check_env();

/// How serious a finding is.
enum class CheckLevel {
    ok,       ///< Fine.
    warning,  ///< Works, but weakens the evidence or a feature.
    failure,  ///< meerkat can't record (properly) until this is fixed.
};

/// One line of the check.
struct CheckItem {
    CheckLevel level = CheckLevel::ok;  ///< How serious.
    std::string topic;                  ///< Short name, e.g. `speedtest`.
    std::string detail;                 ///< What was found, and what to do.
};

/**
 * @brief Runs all checks.
 *
 * In order: config file (found, valid, not readable by others when it holds
 * contract figures), contract figures, the speed-test CLI (installed and
 * really Ookla's), `ping` (installed and allowed to send), `iw`, the data
 * directory (writable, enough space, existing CSVs with the right schema),
 * and the connection (default route, carrier, cable or Wi-Fi, link speed
 * against the contract). Nothing is changed on the system.
 *
 * @param config The effective config (defaults if the file was unusable).
 * @param report How it was obtained.
 * @param env    The system.
 * @return The findings.
 */
std::vector<CheckItem> run_checks(const core::Config& config, const ConfigReport& report,
                                  const CheckEnv& env);

/**
 * @brief Prints findings as `[ ok ] topic  detail` lines plus a summary.
 * @param items The findings.
 * @param out   Where to print.
 */
void print_checks(const std::vector<CheckItem>& items, std::ostream& out);

/**
 * @brief Whether any finding is a failure.
 * @param items The findings.
 * @return True if at least one is CheckLevel::failure.
 */
bool has_failure(const std::vector<CheckItem>& items);

}  // namespace meerkat::app
