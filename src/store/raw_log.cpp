/**
 * @file raw_log.cpp
 * @brief Implementation of the raw speed-test log.
 */
#include "store/raw_log.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace meerkat::store {

namespace {

/**
 * @brief Whether @p s is valid UTF-8, judged the way a strict
 *        nlohmann::json dump judges it.
 * @param s The bytes.
 * @return True if a strict dump keeps them as they are.
 */
bool valid_utf8(const std::string& s) {
    try {
        static_cast<void>(nlohmann::json(s).dump());
        return true;
    } catch (const nlohmann::json::type_error&) {
        return false;
    }
}

}  // namespace

std::string utc_day(core::Timestamp t) { return core::to_iso8601(t).substr(0, 10); }

std::string base64(std::string_view data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    const auto byte = [&data](std::size_t k) {
        return static_cast<unsigned>(static_cast<unsigned char>(data[k]));
    };
    for (; i + 3 <= data.size(); i += 3) {
        const unsigned v = byte(i) << 16 | byte(i + 1) << 8 | byte(i + 2);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (const std::size_t rest = data.size() - i; rest > 0) {
        const unsigned v = byte(i) << 16 | (rest == 2 ? byte(i + 1) << 8u : 0u);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string raw_record(const core::Measurement<core::SpeedResult>& m) {
    const auto& r = m.result;
    nlohmann::ordered_json j;
    j["time"] = core::to_iso8601(m.time);
    j["backend"] = r.backend;
    j["status"] = std::string{core::to_string(r.status)};
    j["error"] = r.error;
    if (valid_utf8(r.raw)) {
        j["output"] = r.raw;
    } else {
        j["output_base64"] = base64(r.raw);  // byte for byte all the same
    }
    // Only our own fields can still hold invalid bytes (the error text may
    // quote the tool in a non-UTF-8 locale): those are replaced with U+FFFD.
    // The output is valid by now, so it is kept exactly.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

RawLog::RawLog(std::filesystem::path dir) : dir_{std::move(dir)} {
    std::filesystem::create_directories(dir_);
}

std::filesystem::path RawLog::file_for(std::string_view day) const {
    return dir_ / (std::string{day} + ".jsonl");
}

void RawLog::append(const core::Measurement<core::SpeedResult>& m) {
    const std::string day = utc_day(m.time);
    if (day != day_ || !file_.is_open()) {
        file_.close();
        day_.clear();
        AppendFile f{file_for(day)};
        if (const std::size_t size = f.size(); size > 0 && f.read(size - 1, 1) != "\n") {
            f.append("\n");  // the previous writer died mid-line
        }
        file_ = std::move(f);
        day_ = day;
    }
    file_.append(raw_record(m) + "\n");
}

}  // namespace meerkat::store
