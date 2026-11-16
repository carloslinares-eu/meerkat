/**
 * @file bbm.hpp
 * @brief Reads the CSV export of the Bundesnetzagentur's Breitbandmessung
 *        desktop app (Festnetz-Check): the official measurements.
 *
 * The format, from a real export (app 3.13.0 on Linux): UTF-8, one header
 * line, then one line per measurement; fields separated by `;`, numbers
 * with a decimal comma (`105,49`), the time as local `DD.MM.YYYY HH:MM:SS`.
 * Columns are found by their header, so a reordered or extended export
 * still reads; see tests/fixtures/bbm/.
 */
#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core/time.hpp"

namespace meerkat::probe {

/// One official measurement from the export.
struct OfficialMeasurement {
    int number = 0;                               ///< `Nr.`: its number in the export.
    core::Timestamp time{};                       ///< `Messzeitpunkt`, converted to UTC.
    std::optional<double> download_mbps;          ///< `Download (Mbit/s)`.
    std::optional<double> upload_mbps;            ///< `Upload (Mbit/s)`.
    std::optional<double> latency_ms;             ///< `Laufzeit (ms)`.
    std::optional<double> contract_down_min;      ///< `Download SOLL minimal`.
    std::optional<double> contract_down_normal;   ///< `Download SOLL normalerweise`.
    std::optional<double> contract_down_max;      ///< `Download SOLL maximal`.
    std::optional<double> contract_up_min;        ///< `Upload SOLL minimal`.
    std::optional<double> contract_up_normal;     ///< `Upload SOLL normalerweise`.
    std::optional<double> contract_up_max;        ///< `Upload SOLL maximal`.
    std::string connection;                       ///< `Festgestellte Art der Anbindung`, e.g. `LAN`.
    std::optional<bool> on_power;                 ///< `Computer an Stromversorgung angeschlossen`.
    std::optional<bool> nic_sufficient;           ///< `Ausreichende Leistung der Netzwerkkarte`.
    std::optional<bool> data_rate_limited;        ///< `Begrenzte Datenübertragungsrate`.
    std::string test_id;                          ///< `Test-ID`: unique per measurement.
    std::string app_version;                      ///< `Desktop-App Versions-Nr.`.
};

/// The text isn't a Breitbandmessung export meerkat can read.
class BbmFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Turns a local wall-clock time into UTC.
 *
 * Arguments: year, month, day, hour, minute, second. Returns nothing if the
 * time doesn't exist locally (skipped by a daylight-saving change).
 */
using LocalToUtc = std::function<std::optional<core::Timestamp>(int, int, int, int, int, int)>;

/**
 * @brief The system's local time zone, as the app on this computer uses it.
 *
 * In the hour repeated when daylight-saving time ends, the export doesn't
 * say which of the two is meant; the C library picks one (mktime()).
 * @return A converter through mktime().
 */
LocalToUtc system_local_to_utc();

/**
 * @brief Reads an export.
 * @param text     The file's content.
 * @param to_utc   Converts the local times.
 * @return Its measurements, in file order.
 * @throws BbmFormatError if a required column (`Messzeitpunkt`,
 *         `Download (Mbit/s)`, `Upload (Mbit/s)`, `Test-ID`) is missing, or a
 *         row has a bad time, number or field count, naming the line.
 */
std::vector<OfficialMeasurement> parse_bbm_csv(std::string_view text, const LocalToUtc& to_utc);

}  // namespace meerkat::probe
