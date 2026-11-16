/**
 * @file bbm.cpp
 * @brief Implementation of the Breitbandmessung export reader.
 */
#include "probe/bbm.hpp"

#include <charconv>
#include <ctime>
#include <map>
#include <utility>

namespace meerkat::probe {

namespace {

/// The columns, by their header in the export.
constexpr std::string_view kNumber = "Nr.";
constexpr std::string_view kTime = "Messzeitpunkt";
constexpr std::string_view kDown = "Download (Mbit/s)";
constexpr std::string_view kDownMin = "Download SOLL minimal (Mbit/s)";
constexpr std::string_view kDownNormal = "Download SOLL normalerweise (Mbit/s)";
constexpr std::string_view kDownMax = "Download SOLL maximal (Mbit/s)";
constexpr std::string_view kUp = "Upload (Mbit/s)";
constexpr std::string_view kUpMin = "Upload SOLL minimal (Mbit/s)";
constexpr std::string_view kUpNormal = "Upload SOLL normalerweise (Mbit/s)";
constexpr std::string_view kUpMax = "Upload SOLL maximal (Mbit/s)";
constexpr std::string_view kLatency = "Laufzeit (ms)";
constexpr std::string_view kPower = "Computer an Stromversorgung angeschlossen";
constexpr std::string_view kConnection = "Festgestellte Art der Anbindung";
constexpr std::string_view kNic = "Ausreichende Leistung der Netzwerkkarte";
constexpr std::string_view kLimited = "Begrenzte Datenübertragungsrate";
constexpr std::string_view kTestId = "Test-ID";
constexpr std::string_view kVersion = "Desktop-App Versions-Nr.";

/**
 * @brief Splits one line at `;`, honouring double quotes (`""` is a quote).
 * @param line The line, without its line end.
 * @return The fields.
 */
std::vector<std::string> split_fields(std::string_view line) {
    std::vector<std::string> out(1);
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                out.back() += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                out.back() += c;
            }
        } else if (c == '"' && out.back().empty()) {
            quoted = true;
        } else if (c == ';') {
            out.emplace_back();
        } else {
            out.back() += c;
        }
    }
    return out;
}

/// @return @p s without spaces and tabs at either end.
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

/**
 * @brief A German decimal number, `105,49` (a `.` is accepted too).
 * @param text The field.
 * @return The number; nothing for an empty field or `-`.
 * @throws std::invalid_argument if it isn't a number.
 */
std::optional<double> german_number(std::string_view text) {
    text = trim(text);
    if (text.empty() || text == "-") {
        return std::nullopt;
    }
    std::string s{text};
    for (auto& c : s) {
        c = c == ',' ? '.' : c;
    }
    double v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size() || v < 0) {
        throw std::invalid_argument("not a number: '" + std::string{text} + "'");
    }
    return v;
}

/// @return `Ja` → true, `Nein` → false, anything else → nothing.
std::optional<bool> yes_no(std::string_view text) {
    text = trim(text);
    if (text == "Ja") {
        return true;
    }
    if (text == "Nein") {
        return false;
    }
    return std::nullopt;
}

/**
 * @brief Reads `DD.MM.YYYY HH:MM:SS`.
 * @param text   The field.
 * @param to_utc The local-to-UTC conversion.
 * @return The time in UTC.
 * @throws std::invalid_argument if it isn't such a time, or doesn't exist locally.
 */
core::Timestamp local_time(std::string_view text, const LocalToUtc& to_utc) {
    text = trim(text);
    int v[6] = {};
    // Positions: DD.MM.YYYY HH:MM:SS
    static constexpr std::size_t kStart[6] = {6, 3, 0, 11, 14, 17};
    static constexpr std::size_t kLen[6] = {4, 2, 2, 2, 2, 2};
    const bool shape = text.size() == 19 && text[2] == '.' && text[5] == '.' && text[10] == ' ' &&
                       text[13] == ':' && text[16] == ':';
    for (std::size_t i = 0; shape && i < 6; ++i) {
        const auto part = text.substr(kStart[i], kLen[i]);
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), v[i]);
        if (ec != std::errc{} || end != part.data() + part.size()) {
            throw std::invalid_argument("not a time (DD.MM.YYYY HH:MM:SS): '" + std::string{text} + "'");
        }
    }
    if (!shape) {
        throw std::invalid_argument("not a time (DD.MM.YYYY HH:MM:SS): '" + std::string{text} + "'");
    }
    const std::chrono::year_month_day ymd{std::chrono::year{v[0]}, std::chrono::month{static_cast<unsigned>(v[1])},
                                          std::chrono::day{static_cast<unsigned>(v[2])}};
    if (!ymd.ok() || v[3] > 23 || v[4] > 59 || v[5] > 59) {
        throw std::invalid_argument("not a valid time: '" + std::string{text} + "'");
    }
    const auto t = to_utc(v[0], v[1], v[2], v[3], v[4], v[5]);
    if (!t) {
        throw std::invalid_argument("'" + std::string{text} + "' doesn't exist in the local time zone");
    }
    return *t;
}

}  // namespace

LocalToUtc system_local_to_utc() {
    return [](int y, int mo, int d, int h, int mi, int s) -> std::optional<core::Timestamp> {
        ::tzset();
        std::tm tm{};
        tm.tm_year = y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = d;
        tm.tm_hour = h;
        tm.tm_min = mi;
        tm.tm_sec = s;
        tm.tm_isdst = -1;  // the zone's rules decide
        std::tm check = tm;
        const std::time_t secs = std::mktime(&check);
        // mktime() normalises a time skipped by the change to summer time
        // into another hour: then it doesn't exist.
        if (secs == -1 || check.tm_hour != h || check.tm_min != mi || check.tm_mday != d) {
            return std::nullopt;
        }
        return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::from_time_t(secs));
    };
}

std::vector<OfficialMeasurement> parse_bbm_csv(std::string_view text, const LocalToUtc& to_utc) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);  // byte-order mark, as some tools save it
    }
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    }
    if (lines.empty()) {
        throw BbmFormatError("empty file: not a Breitbandmessung export");
    }
    const auto header = split_fields(lines[0]);
    std::map<std::string, std::size_t, std::less<>> col;
    for (std::size_t i = 0; i < header.size(); ++i) {
        col.emplace(std::string{trim(header[i])}, i);
    }
    for (const auto required : {kTime, kDown, kUp, kTestId}) {
        if (!col.contains(required)) {
            throw BbmFormatError("not a Breitbandmessung export: no column '" + std::string{required} + "'");
        }
    }

    std::vector<OfficialMeasurement> out;
    for (std::size_t n = 1; n < lines.size(); ++n) {
        if (trim(lines[n]).empty()) {
            continue;
        }
        const auto f = split_fields(lines[n]);
        const auto where = "line " + std::to_string(n + 1) + ": ";
        if (f.size() != header.size()) {
            throw BbmFormatError(where + std::to_string(f.size()) + " fields, the header has " +
                                 std::to_string(header.size()));
        }
        const auto field = [&](std::string_view name) -> std::string_view {
            const auto it = col.find(name);
            return it == col.end() ? std::string_view{} : std::string_view{f[it->second]};
        };
        try {
            OfficialMeasurement m;
            if (const auto nr = german_number(field(kNumber))) {
                m.number = static_cast<int>(*nr);
            }
            m.time = local_time(field(kTime), to_utc);
            m.download_mbps = german_number(field(kDown));
            m.upload_mbps = german_number(field(kUp));
            m.latency_ms = german_number(field(kLatency));
            m.contract_down_min = german_number(field(kDownMin));
            m.contract_down_normal = german_number(field(kDownNormal));
            m.contract_down_max = german_number(field(kDownMax));
            m.contract_up_min = german_number(field(kUpMin));
            m.contract_up_normal = german_number(field(kUpNormal));
            m.contract_up_max = german_number(field(kUpMax));
            m.connection = std::string{trim(field(kConnection))};
            m.on_power = yes_no(field(kPower));
            m.nic_sufficient = yes_no(field(kNic));
            m.data_rate_limited = yes_no(field(kLimited));
            m.test_id = std::string{trim(field(kTestId))};
            m.app_version = std::string{trim(field(kVersion))};
            if (m.test_id.empty()) {
                throw std::invalid_argument("no Test-ID");
            }
            out.push_back(std::move(m));
        } catch (const std::invalid_argument& e) {
            throw BbmFormatError(where + e.what());
        }
    }
    return out;
}

}  // namespace meerkat::probe
