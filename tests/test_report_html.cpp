/**
 * @file test_report_html.cpp
 * @brief Tests for `meerkat report` and the report page (task 3.3), on an
 *        invented data directory with the made-up tariff 250/200/150 down,
 *        50/40/30 up.
 */
#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "app/report.hpp"
#include "core/text.hpp"
#include "core/time.hpp"
#include "report/html.hpp"
#include "store/csv.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"
#include "store/sha256.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace std::chrono_literals;

namespace {

/// 2026-03-10T00:00:00Z plus @p offset.
core::Timestamp at(std::chrono::seconds offset = {}) {
    return core::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 3 / 10}} + offset;
}

/// The made-up contract.
core::Config config_for(const std::filesystem::path& dir) {
    core::Config c;
    c.data_dir = dir;
    c.speed_interval = 20min;
    c.contract.download = core::ContractSpeeds{250, 200, 150};
    c.contract.upload = core::ContractSpeeds{50, 40, 30};
    return c;
}

/// One speed test row.
core::Measurement<core::SpeedResult> test_at(core::Timestamp t, double down, double up,
                                             core::LinkType link = core::LinkType::wired) {
    core::Measurement<core::SpeedResult> m;
    m.time = t;
    m.context.link_type = link;
    m.context.interface = link == core::LinkType::wifi ? "wlan0" : "eth0";
    if (link == core::LinkType::wired) {
        m.context.link_speed_mbps = 1000;
    }
    m.result.status = core::Status::ok;
    m.result.download_mbps = down;
    m.result.upload_mbps = up;
    m.result.server_id = 12345;
    m.result.server_name = "Example Server";
    m.result.server_location = "Example City";
    return m;
}

/**
 * @brief Seven invented days (2026-03-10 to 16, UTC): a test every 20 min
 *        with an evening dip below the minimum, some Wi-Fi tests, one
 *        official pause, connectivity rounds with one outage, and the
 *        manifest of the first day.
 */
void fill(const std::filesystem::path& dir) {
    {
        store::CsvWriter w{dir / store::kSpeedFile, store::speed_schema()};
        for (auto t = at(); t < at(7 * 24h); t += 20min) {
            const auto since = t - at();
            const double hour = std::fmod(static_cast<double>(since.count()) / 3600.0, 24.0);
            const bool evening = hour >= 19 && hour < 22;
            const double wobble = 6 * std::sin(static_cast<double>(since.count()) / 5000.0);
            if (since == 30h) {
                auto paused = test_at(t, 0, 0);
                paused.result = {};
                paused.result.status = core::Status::paused_official;
                w.write_row(store::speed_row(paused));
                continue;
            }
            const auto link = since >= 50h && since < 52h ? core::LinkType::wifi : core::LinkType::wired;
            w.write_row(store::speed_row(test_at(t, (evening ? 120 : 238) + wobble, (evening ? 25 : 46) + wobble / 6, link)));
        }
    }
    {
        store::CsvWriter w{dir / store::kConnectivityFile, store::connectivity_schema()};
        core::NetworkContext ctx;
        ctx.link_type = core::LinkType::wired;
        ctx.interface = "eth0";
        ctx.gateway = "192.0.2.1";
        for (auto t = at(); t < at(7 * 24h); t += 30s) {
            const bool up = !(t >= at(40h + 10min) && t < at(40h + 25min));  // 15 min down
            core::PingResult gw;
            gw.status = core::Status::ok;
            gw.target = "192.0.2.1";
            gw.sent = 3;
            gw.received = 3;
            w.write_row(store::ping_row({t, ctx, gw}));
            core::PingResult pub;
            pub.status = core::Status::ok;  // ping ran; no replies is the answer
            pub.target = "203.0.113.5";
            pub.sent = 3;
            pub.received = up ? 3 : 0;
            w.write_row(store::ping_row({t, ctx, pub}));
            core::DnsResult dns;
            dns.status = up ? core::Status::ok : core::Status::timeout;
            dns.host = "example.com";
            w.write_row(store::dns_row({t, ctx, dns}));
        }
    }
    std::filesystem::create_directories(dir / store::kManifestDir);
    store::write_manifest(dir, store::build_manifest(dir, "2026-03-10", at(24h + 5s),
                                                     store::daily_files("2026-03-10")));
}

/// Runs the command with UTC and a fixed clock.
int run_report(const core::Config& c, const app::ReportOptions& o, std::string& out, std::string& err) {
    app::ReportEnv env;
    env.zone = app::utc_zone();
    env.now = [] { return at(7 * 24h + 2h); };
    env.version = "0.0.0-test";
    std::ostringstream o_out;
    std::ostringstream o_err;
    const int code = app::report_command(c, o, env, o_out, o_err);
    out = o_out.str();
    err = o_err.str();
    return code;
}

/// @return How often @p what occurs in @p text.
std::size_t count(const std::string& text, std::string_view what) {
    std::size_t n = 0;
    for (auto p = text.find(what); p != std::string::npos; p = text.find(what, p + 1)) {
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("meerkat report writes one self-contained page") {
    test::TempDir dir;
    test::TempDir elsewhere;
    fill(dir.path);
    const auto file = elsewhere.path / "report.html";
    std::string out;
    std::string err;
    REQUIRE(run_report(config_for(dir.path), {"2026-03-10", "2026-03-16", file}, out, err) == 0);
    CHECK(err.empty());
    CHECK(out.find("2026-03-10 to 2026-03-16") != std::string::npos);
    CHECK(out.find("Download: ") != std::string::npos);
    CHECK(out.find("1 outage.") != std::string::npos);

    const auto page = *core::text::read_file(file);
    CHECK(page.starts_with("<!doctype html>"));
    CHECK(page.find("<script") == std::string::npos);    // no scripts
    CHECK(page.find("src=") == std::string::npos);       // loads nothing
    CHECK(count(page, "http") == count(page, "http://www.w3.org/2000/svg"));  // only the SVG namespace
    CHECK(count(page, "<svg") == 5);                     // 2 time, 2 hour, outages
    CHECK(page.find("✕ Deviation in") != std::string::npos);  // the evenings fall below the minimum
    CHECK(page.find("Tests over other links") != std::string::npos);
    CHECK(page.find("Wi-Fi") != std::string::npos);
    CHECK(page.find("<code>paused_official</code></td>") != std::string::npos);
    CHECK(page.find("line (beyond the router)") != std::string::npos);
    CHECK(page.find("2026-03-11 16:10") != std::string::npos);  // the outage, UTC here
    CHECK(page.find("Example Server") != std::string::npos);
    CHECK(page.find("1 outage, 15 min in total") != std::string::npos);
    CHECK(page.find("every 20 min") != std::string::npos);
    CHECK(page.find("1000 Mbit/s") != std::string::npos);
    // The manifest of the first day, by its hash; later days aren't sealed.
    const auto manifest = store::sha256_file(store::manifest_path(dir.path, "2026-03-10"));
    REQUIRE(manifest);
    CHECK(page.find(*manifest) != std::string::npos);
    CHECK(page.find("no manifest (the day isn") != std::string::npos);
    CHECK(page.find("Every row of the data files, read up to a day past the period, could be read.") != std::string::npos);
    CHECK((std::filesystem::status(file).permissions() & std::filesystem::perms::others_read) ==
          std::filesystem::perms::none);
    // No temporary file left behind.
    std::size_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(elsewhere.path)) {
        files += e.path().filename().string().starts_with(".report.html") ? 1U : 0U;
    }
    CHECK(files == 0);
}

TEST_CASE("meerkat report: the default range starts at the first test") {
    test::TempDir dir;
    test::TempDir elsewhere;
    fill(dir.path);
    std::string out;
    std::string err;
    REQUIRE(run_report(config_for(dir.path), {"", "2026-03-12", elsewhere.path / "r.html"}, out, err) == 0);
    CHECK(out.find("(2026-03-10 to 2026-03-12, UTC)") != std::string::npos);
}

TEST_CASE("meerkat report: bad dates and ranges") {
    test::TempDir dir;
    std::string out;
    std::string err;
    CHECK(run_report(config_for(dir.path), {"2026-3-1", "", dir.path / "r.html"}, out, err) == 2);
    CHECK(err.find("--from must be a date") != std::string::npos);
    CHECK(run_report(config_for(dir.path), {"", "tomorrow\x1b", dir.path / "r.html"}, out, err) == 2);
    CHECK(err.find("tomorrow\\x1b") != std::string::npos);
    CHECK(run_report(config_for(dir.path), {"2026-03-12", "2026-03-10", dir.path / "r.html"}, out, err) == 2);
    CHECK(err.find("is after --to") != std::string::npos);
    CHECK(run_report(config_for(dir.path), {"2025-03-01", "2026-03-10", dir.path / "r.html"}, out, err) == 2);
    CHECK(err.find("longer than 366 days") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(dir.path / "r.html"));
}

TEST_CASE("meerkat report won't write into the data directory (#83)") {
    test::TempDir dir;
    const auto data = dir.path / "data";
    std::filesystem::create_directories(data / "raw");
    fill(data);
    const auto before = *core::text::read_file(data / store::kSpeedFile);
    std::string out;
    std::string err;
    CHECK(run_report(config_for(data), {"2026-03-10", "2026-03-11", data / store::kSpeedFile}, out, err) == 2);
    CHECK(run_report(config_for(data), {"2026-03-10", "2026-03-11", data / "raw" / "r.html"}, out, err) == 2);
    CHECK(run_report(config_for(data), {"2026-03-10", "2026-03-11", data / ".." / "data" / "r.html"}, out, err) == 2);
    CHECK(err.find("won't write the report into the data directory") != std::string::npos);
    CHECK(*core::text::read_file(data / store::kSpeedFile) == before);
    CHECK(run_report(config_for(data), {"2026-03-10", "2026-03-11", dir.path / "r.html"}, out, err) == 0);
}

TEST_CASE("meerkat report: no data is a report that says so; a bad output dir is an error") {
    test::TempDir dir;
    test::TempDir elsewhere;
    std::string out;
    std::string err;
    REQUIRE(run_report(config_for(dir.path), {"2026-03-10", "2026-03-11", elsewhere.path / "r.html"}, out, err) == 0);
    CHECK(out.find("No speed.csv") != std::string::npos);
    const auto page = *core::text::read_file(elsewhere.path / "r.html");
    CHECK(page.find("No speed.csv in the data directory") != std::string::npos);
    CHECK(page.find("No data in this period") != std::string::npos);
    CHECK(page.find("No connectivity.csv") != std::string::npos);

    CHECK(run_report(config_for(dir.path), {"2026-03-10", "2026-03-11", elsewhere.path / "missing" / "r.html"}, out, err) == 1);
    CHECK(err.find("can't create") != std::string::npos);
}

TEST_CASE("render_html escapes what comes from the data") {
    report::SpeedData speed;
    speed.file_exists = true;
    report::SpeedRow r;
    r.time = at(1h);
    r.status = core::Status::ok;
    r.link_type = core::LinkType::wired;
    r.interface = "eth0";
    r.download_mbps = 200;
    r.upload_mbps = 40;
    r.server_name = "<script>alert(1)</script>";
    r.server_location = "A&B\x1b[31m";
    speed.rows = {r};
    report::ReportMeta meta;
    meta.period = {at(), at(24h)};
    meta.first_day = std::chrono::sys_days{std::chrono::year{2026} / 3 / 10};
    meta.last_day = meta.first_day;
    meta.zone = "UTC";
    const auto analysis = report::analyse(speed, {}, {}, report::utc_local(), meta.period);
    const auto page = report::render_html(meta, speed, {}, analysis, report::utc_local());
    CHECK(page.find("<script>") == std::string::npos);
    CHECK(page.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos);
    CHECK(page.find("A&amp;B\\x1b[31m") != std::string::npos);
    CHECK(page.find("⚠ No contract figures") != std::string::npos);
}

TEST_CASE("utc_zone: days start at UTC midnight") {
    const auto z = app::utc_zone();
    CHECK(z.midnight(std::chrono::sys_days{std::chrono::year{2026} / 3 / 10}) == at());
    CHECK(z.name == "UTC");
}
