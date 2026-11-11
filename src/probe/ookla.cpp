/**
 * @file ookla.cpp
 * @brief Implementation of the Ookla speed-test backend.
 */
#include "probe/ookla.hpp"

#include <exception>
#include <initializer_list>
#include <utility>

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
const json* find(const json& j, std::initializer_list<const char*> path) {
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
std::optional<double> number(const json& j, std::initializer_list<const char*> path) {
    const json* v = find(j, path);
    if (v == nullptr || !v->is_number()) {
        return std::nullopt;
    }
    return v->get<double>();
}

/**
 * @brief Reads a string at @p path.
 * @param j    The object.
 * @param path Keys.
 * @return The string, or empty if missing or not a string.
 */
std::string string(const json& j, std::initializer_list<const char*> path) {
    const json* v = find(j, path);
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
        if (!j.is_discarded() && string(j, {"type"}) == "result") {
            result = std::move(j);
            result_line = line;
        }
    });
    if (result_line.empty()) {
        r.error = "no result in the speedtest output";
        return r;
    }
    r.raw = std::string{result_line};
    if (const auto bw = number(result, {"download", "bandwidth"}); bw && *bw >= 0) {
        r.download_mbps = core::bytes_per_second_to_mbps(*bw);
    }
    if (const auto bw = number(result, {"upload", "bandwidth"}); bw && *bw >= 0) {
        r.upload_mbps = core::bytes_per_second_to_mbps(*bw);
    }
    r.latency_ms = number(result, {"ping", "latency"});
    r.jitter_ms = number(result, {"ping", "jitter"});
    r.packet_loss_percent = number(result, {"packetLoss"});
    if (const json* id = find(result, {"server", "id"}); id != nullptr && id->is_number_integer()) {
        r.server_id = id->get<std::int64_t>();
    }
    r.server_name = string(result, {"server", "name"});
    r.server_location = string(result, {"server", "location"});
    r.isp = string(result, {"isp"});
    r.external_ip = string(result, {"interface", "externalIp"});
    r.result_url = string(result, {"result", "url"});
    if (r.download_mbps && r.upload_mbps) {
        r.status = core::Status::ok;
    } else {
        r.error = "the speedtest result has no download or upload speed";
    }
    return r;
}

std::string ookla_errors(std::string_view text) {
    static constexpr std::string_view kPlainTag = "] [error] ";
    std::string out;
    const auto add = [&out](std::string_view msg) {
        msg = trim(msg);
        if (!msg.empty()) {
            out += out.empty() ? "" : "; ";
            out += msg;
        }
    };
    core::text::for_each_line(text, [&](std::string_view line) {
        line = trim(line);
        if (line.starts_with('{')) {
            const auto j = parse_line(line);
            if (!j.is_discarded() && string(j, {"type"}) == "log" &&
                string(j, {"level"}) == "error") {
                add(string(j, {"message"}));
            }
        } else if (const auto pos = line.find(kPlainTag);
                   line.starts_with('[') && pos != std::string_view::npos) {
            add(line.substr(pos + kPlainTag.size()));
        }
    });
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
    }

    switch (proc.outcome) {
        case sys::ProcessResult::Outcome::spawn_failed:
            r.status = proc.not_found ? core::Status::not_available : core::Status::error;
            r.error = proc.not_found ? "the Ookla speedtest CLI is not installed "
                                       "(https://www.speedtest.net/apps/cli)"
                                     : proc.error;
            return r;
        case sys::ProcessResult::Outcome::timed_out:
            r.status = core::Status::timeout;
            r.error = "speedtest gave no result within " +
                      std::to_string(options_.timeout.count()) + " s";
            r.raw = std::string{trim(proc.out)};
            return r;
        case sys::ProcessResult::Outcome::signaled:
            r.error = "speedtest was killed by signal " + std::to_string(proc.signal);
            return r;
        case sys::ProcessResult::Outcome::exited:
            break;
    }

    const auto errors = ookla_errors(proc.err);
    auto parsed = parse_ookla(proc.out);
    if (parsed.status == core::Status::ok) {
        if (proc.exit_code != 0) {
            parsed.error = errors.empty()
                               ? "speedtest exited with code " + std::to_string(proc.exit_code)
                               : errors;
        }
        return parsed;
    }
    // Keep whatever was found, plus the CLI's own words for the audit log.
    if (parsed.raw.empty()) {
        parsed.raw = std::string{trim(proc.err)};
    }
    if (!errors.empty()) {
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
