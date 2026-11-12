/**
 * @file ookla.cpp
 * @brief Implementation of the Ookla speed-test backend.
 */
#include "probe/ookla.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/text.hpp"

namespace meerkat::probe {

namespace {

using nlohmann::json;
using core::text::trim;

/**
 * @brief Follows a path of object keys.
 * @param j    Where to start.
 * @param path Keys, outermost first.
 * @return The value, or nullptr if any step is missing or not an object.
 */
const json* json_at(const json& j, std::initializer_list<const char*> path) {
    const json* cur = &j;
    for (const char* key : path) {
        if (!cur->is_object()) {
            return nullptr;
        }
        const auto it = cur->find(key);
        if (it == cur->end()) {
            return nullptr;
        }
        cur = &*it;
    }
    return cur;
}

/**
 * @brief Reads a number at @p path.
 * @param j    The object.
 * @param path Keys.
 * @return The number, or empty if missing or not a number.
 */
std::optional<double> json_number(const json& j, std::initializer_list<const char*> path) {
    const json* v = json_at(j, path);
    if (v == nullptr || !v->is_number()) {
        return std::nullopt;
    }
    return v->get<double>();
}

/**
 * @brief Reads a number at @p path that must lie in [@p lo, @p hi].
 * @param j    The object.
 * @param path Keys.
 * @param lo   Smallest valid value.
 * @param hi   Largest valid value.
 * @return The number, or empty if missing, not a number or out of range.
 */
std::optional<double> json_number_in(const json& j, std::initializer_list<const char*> path,
                                     double lo, double hi) {
    const auto v = json_number(j, path);
    return v && *v >= lo && *v <= hi ? v : std::nullopt;
}

/**
 * @brief Reads a string at @p path.
 * @param j    The object.
 * @param path Keys.
 * @return The string, or empty if missing or not a string.
 */
std::string json_string(const json& j, std::initializer_list<const char*> path) {
    const json* v = json_at(j, path);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string{};
}

/**
 * @brief Parses @p line as JSON without throwing.
 * @param line The text.
 * @return The value; `is_discarded()` if it isn't valid JSON.
 */
json parse_line(std::string_view line) {
    return json::parse(line.begin(), line.end(), nullptr, /*allow_exceptions=*/false);
}

/**
 * @brief Recognises the CLI's messages for "no network at all".
 * @param errors Messages from ookla_errors().
 * @return True if a host name couldn't be resolved (the CLI's first step
 *         is fetching its configuration by name).
 */
bool looks_offline(std::string_view errors) {
    return errors.find("HostNotFoundException") != std::string_view::npos ||
           errors.find("Couldn't resolve host name") != std::string_view::npos;
}

/**
 * @brief Recognises the usage error of the Python `speedtest-cli`, which is
 *        also installed as `speedtest`.
 * @param err stderr.
 * @return True if it looks like that tool rejected `--format`.
 */
bool looks_like_python_speedtest_cli(std::string_view err) {
    return err.find("unrecognized arguments") != std::string_view::npos ||
           err.find("usage: speedtest") != std::string_view::npos;
}

}  // namespace

core::SpeedResult parse_ookla(std::string_view text) {
    core::SpeedResult r;
    r.backend = "ookla";
    json result;
    std::string_view result_line;
    core::text::for_each_line(text, [&](std::string_view line) {
        line = trim(line);
        if (line.empty() || line.front() != '{') {
            return;
        }
        auto j = parse_line(line);
        if (!j.is_discarded() && json_string(j, {"type"}) == "result") {
            result = std::move(j);
            result_line = line;
        }
    });
    if (result_line.empty()) {
        r.error = "no result in the speedtest output";
        return r;
    }
    r.raw = std::string{result_line};
    // A bandwidth of 0 means the phase didn't produce a figure: missing, never 0.
    if (const auto bw = json_number(result, {"download", "bandwidth"}); bw && *bw > 0) {
        r.download_mbps = core::bytes_per_second_to_mbps(*bw);
    }
    if (const auto bw = json_number(result, {"upload", "bandwidth"}); bw && *bw > 0) {
        r.upload_mbps = core::bytes_per_second_to_mbps(*bw);
    }
    r.latency_ms = json_number_in(result, {"ping", "latency"}, 0, 1e6);
    r.jitter_ms = json_number_in(result, {"ping", "jitter"}, 0, 1e6);
    r.packet_loss_percent = json_number_in(result, {"packetLoss"}, 0, 100);
    if (const json* id = json_at(result, {"server", "id"}); id != nullptr) {
        if (id->is_number_unsigned()) {
            if (const auto u = id->get<std::uint64_t>();
                u > 0 && u <= static_cast<std::uint64_t>(INT64_MAX)) {
                r.server_id = static_cast<std::int64_t>(u);
            }
        } else if (id->is_number_integer() && id->get<std::int64_t>() > 0) {
            r.server_id = id->get<std::int64_t>();
        }
    }
    r.server_name = json_string(result, {"server", "name"});
    r.server_location = json_string(result, {"server", "location"});
    r.isp = json_string(result, {"isp"});
    r.external_ip = json_string(result, {"interface", "externalIp"});
    r.result_url = json_string(result, {"result", "url"});
    if (r.download_mbps && r.upload_mbps) {
        r.status = core::Status::ok;
    } else {
        r.error = "the speedtest result has no download or upload speed";
    }
    return r;
}

std::string ookla_errors(std::string_view text) {
    static constexpr std::string_view kPlainTag = "] [error] ";
    std::vector<std::string> messages;
    const auto add = [&messages](std::string_view msg) {
        msg = trim(msg);
        // The CLI often repeats a line; keep each message once.
        if (!msg.empty() && std::find(messages.begin(), messages.end(), msg) == messages.end()) {
            messages.emplace_back(msg);
        }
    };
    core::text::for_each_line(text, [&](std::string_view line) {
        line = trim(line);
        if (line.starts_with('{')) {
            const auto j = parse_line(line);
            if (!j.is_discarded() && json_string(j, {"type"}) == "log" &&
                json_string(j, {"level"}) == "error") {
                add(json_string(j, {"message"}));
            }
        } else if (const auto pos = line.find(kPlainTag);
                   line.starts_with('[') && pos != std::string_view::npos) {
            add(line.substr(pos + kPlainTag.size()));
        }
    });
    std::string out;
    for (const auto& m : messages) {
        out += out.empty() ? "" : "; ";
        out += m;
    }
    return out;
}

OoklaBackend::OoklaBackend(OoklaOptions options, sys::Runner runner)
    : options_(std::move(options)), runner_(std::move(runner)) {}

core::SpeedResult OoklaBackend::measure() {
    core::SpeedResult r;
    r.backend = std::string{name()};
    if (options_.server_id && *options_.server_id <= 0) {
        r.error = "invalid speedtest server id " + std::to_string(*options_.server_id);
        return r;
    }
    sys::Command cmd{.program = options_.program,
                     .args = {"--format=json", "--accept-license", "--accept-gdpr"},
                     .timeout = options_.timeout,
                     .env = {{"LC_ALL", "C"}}};
    if (options_.server_id) {
        cmd.args.push_back("--server-id=" + std::to_string(*options_.server_id));
    }

    sys::ProcessResult proc;
    try {
        proc = runner_(cmd);
    } catch (const std::exception& e) {
        r.error = std::string{"cannot run speedtest: "} + e.what();
        return r;
    } catch (...) {
        r.error = "cannot run speedtest: unknown exception";
        return r;
    }
    const auto errors = ookla_errors(proc.err);
    const auto with_cli_errors = [&errors](std::string text) {
        return errors.empty() ? text : text + " (" + errors + ")";
    };
    const auto raw_output = [&proc] {
        const auto out = trim(proc.out);
        return std::string{out.empty() ? trim(proc.err) : out};
    };

    switch (proc.outcome) {
        case sys::ProcessResult::Outcome::spawn_failed:
            r.status = proc.not_found ? core::Status::not_available : core::Status::error;
            r.error = proc.not_found ? "the Ookla speedtest CLI is not installed "
                                       "(https://www.speedtest.net/apps/cli)"
                                     : proc.error;
            return r;
        case sys::ProcessResult::Outcome::timed_out:
            r.status = core::Status::timeout;
            r.error = with_cli_errors("speedtest gave no result within " +
                                      std::to_string(options_.timeout.count()) + " s");
            r.raw = raw_output();
            return r;
        case sys::ProcessResult::Outcome::signaled:
            r.error = with_cli_errors("speedtest was killed by signal " + std::to_string(proc.signal));
            r.raw = raw_output();
            return r;
        case sys::ProcessResult::Outcome::exited:
            break;
    }

    auto parsed = parse_ookla(proc.out);
    if (parsed.status == core::Status::ok) {
        // A valid result stays ok; complaints from the CLI are kept with it.
        if (!errors.empty()) {
            parsed.error = errors;
        } else if (proc.exit_code != 0) {
            parsed.error = "speedtest exited with code " + std::to_string(proc.exit_code);
        }
        return parsed;
    }
    // Keep whatever was found, plus the CLI's own words for the audit log.
    if (parsed.raw.empty()) {
        parsed.raw = std::string{trim(proc.err)};
    }
    if (!errors.empty()) {
        parsed.status = looks_offline(errors) ? core::Status::no_network : core::Status::error;
        parsed.error = errors;
    } else if (looks_like_python_speedtest_cli(proc.err)) {
        parsed.error = "'" + options_.program +
                       "' is not the Ookla CLI (the Python speedtest-cli is installed under the "
                       "same name)";
    } else if (proc.exit_code != 0) {
        parsed.error += " (exit code " + std::to_string(proc.exit_code) + ")";
    }
    return parsed;
}

}  // namespace meerkat::probe
