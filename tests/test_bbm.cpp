/**
 * @file test_bbm.cpp
 * @brief Tests for reading the Breitbandmessung app's CSV export (task 3.6).
 */
#include <doctest/doctest.h>

#include <cstdlib>
#include <ctime>
#include <string>

#include "probe/bbm.hpp"
#include "support.hpp"

using namespace std::chrono_literals;
using meerkat::core::Timestamp;
using meerkat::probe::BbmFormatError;
using meerkat::probe::LocalToUtc;
using meerkat::probe::parse_bbm_csv;

namespace {

/// Local time is UTC+1 (as in Germany in winter), for the parse tests.
const LocalToUtc kPlusOne = [](int y, int mo, int d, int h, int mi, int s) -> std::optional<Timestamp> {
    const std::chrono::year_month_day ymd{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(mo)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
    return Timestamp{std::chrono::sys_days{ymd} + std::chrono::hours{h} + std::chrono::minutes{mi} +
                     std::chrono::seconds{s} - 1h};
};

/// The header of a real export.
const std::string kHeader =
    "Nr.;Messzeitpunkt;Download (Mbit/s);Download SOLL minimal (Mbit/s);Download SOLL normalerweise (Mbit/s);"
    "Download SOLL maximal (Mbit/s);Upload (Mbit/s);Upload SOLL minimal (Mbit/s);Upload SOLL normalerweise "
    "(Mbit/s);Upload SOLL maximal (Mbit/s);Laufzeit (ms);Computer an Stromversorgung angeschlossen;"
    "Festgestellte Art der Anbindung;Ausreichende Leistung der Netzwerkkarte;Anbieter;Tarif;Tarif-Auswahl;"
    "Begrenzte Datenübertragungsrate;Test-ID;Desktop-App Versions-Nr.;Betriebssystem";

/// @return A row of the export with @p time and @p down; the rest as in single.csv.
std::string row(const std::string& time, const std::string& down) {
    return "1;" + time + ";" + down +
           ";150;200;250;46,08;30;40;50;14;Ja;LAN;Ja;Example ISP;Example 250;Liste;Nein;abc;3.13.0 64-Bit;Linux";
}

}  // namespace

TEST_CASE("parse_bbm_csv: one measurement, as the app exports it") {
    const auto m = parse_bbm_csv(meerkat::test::fixture("bbm/single.csv"), kPlusOne);
    REQUIRE(m.size() == 1);
    CHECK(m[0].number == 1);
    CHECK(meerkat::core::to_iso8601(m[0].time) == "2026-03-10T18:25:12Z");
    CHECK(m[0].download_mbps == doctest::Approx(187.42));
    CHECK(m[0].upload_mbps == doctest::Approx(46.08));
    CHECK(m[0].latency_ms == doctest::Approx(14));
    CHECK(m[0].contract_down_min == 150);
    CHECK(m[0].contract_down_normal == 200);
    CHECK(m[0].contract_down_max == 250);
    CHECK(m[0].contract_up_min == 30);
    CHECK(m[0].contract_up_normal == 40);
    CHECK(m[0].contract_up_max == 50);
    CHECK(m[0].connection == "LAN");
    CHECK(m[0].on_power == true);
    CHECK(m[0].nic_sufficient == true);
    CHECK(m[0].data_rate_limited == false);
    CHECK(m[0].test_id == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    CHECK(m[0].app_version == "3.13.0 64-Bit");
}

TEST_CASE("parse_bbm_csv: byte-order mark, CRLF, several rows") {
    const auto m = parse_bbm_csv(meerkat::test::fixture("bbm/three.csv"), kPlusOne);
    REQUIRE(m.size() == 3);
    CHECK(m[1].number == 2);
    CHECK(m[1].download_mbps == doctest::Approx(142.05));
    CHECK(m[2].download_mbps == doctest::Approx(241.9));
    CHECK(m[2].upload_mbps == 48);
    CHECK(m[2].connection == "WLAN");
    CHECK(m[2].on_power == false);
}

TEST_CASE("parse_bbm_csv: columns by name, missing values stay empty") {
    // Reordered, without the optional columns, with a quoted field and an empty upload.
    const auto m = parse_bbm_csv("Test-ID;Upload (Mbit/s);Messzeitpunkt;Download (Mbit/s)\n"
                                 "\"x;1\";;01.02.2026 00:30:00;-\n",
                                 kPlusOne);
    REQUIRE(m.size() == 1);
    CHECK(m[0].test_id == "x;1");
    CHECK_FALSE(m[0].upload_mbps);
    CHECK_FALSE(m[0].download_mbps);
    CHECK_FALSE(m[0].contract_down_min);
    CHECK_FALSE(m[0].on_power);
    CHECK(meerkat::core::to_iso8601(m[0].time) == "2026-01-31T23:30:00Z");
}

TEST_CASE("parse_bbm_csv: what isn't an export, or a broken row, names the problem") {
    CHECK_THROWS_WITH_AS(parse_bbm_csv("", kPlusOne), doctest::Contains("empty file"), BbmFormatError);
    CHECK_THROWS_WITH_AS(parse_bbm_csv("a,b\n1,2\n", kPlusOne), doctest::Contains("no column 'Messzeitpunkt'"),
                         BbmFormatError);
    CHECK_THROWS_WITH_AS(parse_bbm_csv(kHeader + "\n" + row("10.03.2026 19:25:12", "fast"), kPlusOne),
                         doctest::Contains("line 2: not a number: 'fast'"), BbmFormatError);
    CHECK_THROWS_WITH_AS(parse_bbm_csv(kHeader + "\n" + row("2026-03-10 19:25", "1"), kPlusOne),
                         doctest::Contains("line 2: not a time"), BbmFormatError);
    CHECK_THROWS_WITH_AS(parse_bbm_csv(kHeader + "\n" + row("31.02.2026 19:25:12", "1"), kPlusOne),
                         doctest::Contains("not a valid time"), BbmFormatError);
    CHECK_THROWS_WITH_AS(parse_bbm_csv(kHeader + "\n" + row("10.03.2026 19:25:12", "1") + ";extra", kPlusOne),
                         doctest::Contains("line 2: 22 fields, the header has 21"), BbmFormatError);
    CHECK(parse_bbm_csv(kHeader + "\n\n", kPlusOne).empty());  // no measurements: nothing, no error
}

TEST_CASE("system_local_to_utc: German time with its daylight-saving changes") {
    const char* old = std::getenv("TZ");
    const std::string saved = old != nullptr ? old : "";
    // A POSIX rule, so no zone database is needed.
    ::setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    ::tzset();
    const auto to_utc = meerkat::probe::system_local_to_utc();
    CHECK(meerkat::core::to_iso8601(*to_utc(2026, 3, 10, 19, 25, 12)) == "2026-03-10T18:25:12Z");
    CHECK(meerkat::core::to_iso8601(*to_utc(2026, 7, 16, 14, 25, 12)) == "2026-07-16T12:25:12Z");
    CHECK_FALSE(to_utc(2026, 3, 29, 2, 30, 0));  // skipped by the change to summer time
    if (old != nullptr) {
        ::setenv("TZ", saved.c_str(), 1);
    } else {
        ::unsetenv("TZ");
    }
    ::tzset();
}
