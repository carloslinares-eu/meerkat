/**
 * @file ookla.hpp
 * @brief Speed tests with the official Ookla `speedtest` CLI.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/measurement.hpp"
#include "probe/speed.hpp"
#include "sys/process.hpp"

namespace meerkat::probe {

/// Settings for OoklaBackend.
struct OoklaOptions {
    std::string program = "speedtest";            ///< Program name (searched on PATH) or full path.
    std::optional<std::int64_t> server_id;        ///< Pinned server (`--server-id`); empty: Ookla picks one.
    std::chrono::seconds timeout{120};            ///< Longest run before the CLI is stopped.
};

/**
 * @brief Parses the stdout of `speedtest --format=json`.
 *
 * Uses the last line whose JSON `type` is `result`. Ookla reports
 * `bandwidth` in bytes/s; it is converted to Mbit/s. Fields that are missing
 * stay empty. `raw` is that result line, unchanged.
 *
 * @param text stdout of the CLI.
 * @return `ok` if the result has both a download and an upload speed;
 *         otherwise `error` (with whatever was found).
 */
core::SpeedResult parse_ookla(std::string_view text);

/**
 * @brief Collects the error messages the CLI printed to stderr.
 *
 * Understands both forms the CLI uses: JSON log lines
 * (`{"type":"log",…,"level":"error","message":"…"}`) and plain lines
 * (`[time] [error] text`). Other text, such as the licence notice printed on
 * the first run, is ignored.
 *
 * @param text stderr of the CLI.
 * @return The messages joined with `; `, or empty if there are none.
 */
std::string ookla_errors(std::string_view text);

/**
 * @brief Runs the Ookla `speedtest` CLI (1.x) and turns its JSON into a
 *        core::SpeedResult.
 *
 * Command: `speedtest --format=json --accept-license --accept-gdpr
 * [--server-id=N]`, without a shell, in the C locale. meerkat doesn't ship
 * the CLI; by running it, the user accepts Ookla's licence and privacy terms.
 *
 * Status mapping:
 * - `ok`: a result with download and upload. If the CLI also exited with an
 *   error, that text is in `error`.
 * - `not_available`: the program isn't installed.
 * - `timeout`: no result within OoklaOptions::timeout.
 * - `error`: anything else, with the CLI's own message. This includes a
 *   `speedtest` that is the unrelated Python `speedtest-cli`, which doesn't
 *   understand `--format`.
 *
 * Being offline shows up as `error` with Ookla's message; whether the network
 * was there is recorded separately (route and link) by the caller.
 */
class OoklaBackend final : public SpeedBackend {
public:
    /**
     * @brief Creates the backend.
     * @param options Program, server and timeout.
     * @param runner  How to run the CLI; tests pass a fake.
     */
    explicit OoklaBackend(OoklaOptions options = {}, sys::Runner runner = sys::run);

    /// @copydoc SpeedBackend::name
    [[nodiscard]] std::string_view name() const override { return "ookla"; }

    /// @copydoc SpeedBackend::measure
    [[nodiscard]] core::SpeedResult measure() override;

private:
    OoklaOptions options_;  ///< Settings.
    sys::Runner runner_;    ///< Process runner.
};

}  // namespace meerkat::probe
