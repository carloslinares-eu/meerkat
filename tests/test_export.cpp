/**
 * @file test_export.cpp
 * @brief Tests for `meerkat export` (task 3.4), on an invented data
 *        directory with the made-up tariff 250/200/150 down, 50/40/30 up.
 */
#include <doctest/doctest.h>

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "app/export.hpp"
#include "app/verify.hpp"
#include "core/text.hpp"
#include "store/csv.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"
#include "store/sha256.hpp"
#include "support.hpp"
#include "zip_reader.hpp"

using namespace meerkat;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

/// 2026-03-10T00:00:00Z plus @p offset.
core::Timestamp at(std::chrono::seconds offset = {}) {
    return core::Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 3 / 10}} + offset;
}

/// The made-up contract.
core::Config config_for(const fs::path& dir) {
    core::Config c;
    c.data_dir = dir;
    c.speed_interval = 1h;
    c.contract.download = core::ContractSpeeds{250, 200, 150};
    c.contract.upload = core::ContractSpeeds{50, 40, 30};
    return c;
}

/// Appends @p text to @p p.
void append(const fs::path& p, const std::string& text) {
    std::ofstream out{p, std::ios::binary | std::ios::app};
    out << text;
}

/// @return The whole content of @p p.
std::string slurp(const fs::path& p) { return core::text::read_file(p).value(); }

/**
 * @brief Three invented days (2026-03-10 to 12, UTC): a speed test an hour,
 *        a raw log per day, the first two days sealed, and half a row at
 *        the end of connectivity.csv (a write in progress).
 * @param dir     The data directory.
 * @param torn_raw End every raw log with half a line before it is sealed
 *                 (a test cut short by a crash).
 */
void fill(const fs::path& dir, bool torn_raw = false) {
    {
        store::CsvWriter w{dir / store::kSpeedFile, store::speed_schema()};
        for (auto t = at(); t < at(72h); t += 1h) {
            core::Measurement<core::SpeedResult> m;
            m.time = t;
            m.context.link_type = core::LinkType::wired;
            m.context.interface = "eth0";
            m.result.status = core::Status::ok;
            m.result.download_mbps = 230;
            m.result.upload_mbps = 45;
            w.write_row(store::speed_row(m));
        }
    }
    {
        store::CsvWriter w{dir / store::kConnectivityFile, store::connectivity_schema()};
        core::NetworkContext ctx;
        ctx.link_type = core::LinkType::wired;
        ctx.interface = "eth0";
        for (auto t = at(); t < at(72h); t += 10min) {
            core::PingResult p;
            p.status = core::Status::ok;
            p.target = "203.0.113.5";
            p.sent = 3;
            p.received = 3;
            w.write_row(store::ping_row({t, ctx, p}));
        }
    }
    fs::create_directories(dir / store::kRawDir);
    for (const auto* day : {"2026-03-10", "2026-03-11", "2026-03-12"}) {
        append(dir / store::kRawDir / (std::string{day} + ".jsonl"),
               std::string{"{\"time\":\""} + day + "T12:00:00Z\"}\n" + (torn_raw ? "{\"ti" : ""));
    }
    for (const auto* day : {"2026-03-10", "2026-03-11"}) {
        const auto d = store::parse_day(day).value();
        store::write_manifest(dir, store::build_manifest(dir, day, core::Timestamp{d + std::chrono::days{1}},
                                                         store::daily_files(day)));
    }
    append(dir / store::kConnectivityFile, "2026-03-12T23:59:30Z,eth0");  // no line end yet
}

/// Runs the command with a fixed clock (2026-03-12 18:00 UTC), in UTC or
/// @p offset ahead of it.
int run_export(const core::Config& c, const app::ReportOptions& o, std::string& out, std::string& err,
               std::chrono::seconds offset = {}) {
    app::ReportEnv env;
    env.zone = app::utc_zone();
    if (offset != std::chrono::seconds{}) {
        env.zone.name = "Test/Ahead";
        env.zone.local = [offset](core::Timestamp t) {
            const auto wall = t + offset;
            const auto day = std::chrono::floor<std::chrono::days>(wall);
            return report::LocalTime{
                day, static_cast<int>(std::chrono::floor<std::chrono::hours>(wall - day).count()), offset};
        };
    }
    env.now = [] { return at(66h); };
    env.version = "0.0.0-test";
    std::ostringstream o_out;
    std::ostringstream o_err;
    const int code = app::export_command(c, o, env, o_out, o_err);
    out = o_out.str();
    err = o_err.str();
    return code;
}

/// @return The entries of the ZIP at @p p, by name.
std::map<std::string, std::string> unzip(const fs::path& p) {
    std::map<std::string, std::string> m;
    for (auto& e : test::read_zip(slurp(p))) {
        m.emplace(e.name, std::move(e.data));
    }
    return m;
}

}  // namespace

TEST_CASE("meerkat export writes the bundle") {
    test::TempDir data;
    test::TempDir outdir;
    fill(data.path);
    const auto zip_path = outdir.path / "bundle.zip";
    std::string out;
    std::string err;
    REQUIRE(run_export(config_for(data.path), {"2026-03-10", "2026-03-12", zip_path}, out, err) == 0);
    CHECK(err.empty());
    CHECK(out.find("Wrote ") != std::string::npos);
    CHECK(out.find("report 2026-03-10 to 2026-03-12, 7 data files") != std::string::npos);
    CHECK(out.find("Download: 72 cable tests") != std::string::npos);
    CHECK(out.find("Evidence trail: OK") != std::string::npos);
    CHECK(out.find("Left out the last 25 bytes of connectivity.csv: a line still being written.") !=
          std::string::npos);
    CHECK(out.find("No speed.csv") == std::string::npos);

    struct stat st{};
    REQUIRE(::stat(zip_path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(std::distance(fs::directory_iterator{outdir.path}, fs::directory_iterator{}) == 1);  // no temp file

    const auto z = unzip(zip_path);
    const std::string f = "meerkat-export-2026-03-10-to-2026-03-12/";
    CHECK(z.size() == 11);
    for (const auto* name : {"README.txt", "report.html", "verify.txt", "SHA256SUMS", "data/speed.csv",
                             "data/connectivity.csv", "data/raw/2026-03-10.jsonl", "data/raw/2026-03-11.jsonl",
                             "data/raw/2026-03-12.jsonl", "data/manifest/2026-03-10.sha256",
                             "data/manifest/2026-03-11.sha256"}) {
        CAPTURE(name);
        CHECK(z.count(f + name) == 1);
    }

    // The data byte for byte; the file being written up to its last full line.
    CHECK(z.at(f + "data/speed.csv") == slurp(data.path / "speed.csv"));
    CHECK(z.at(f + "data/manifest/2026-03-11.sha256") == slurp(data.path / "manifest/2026-03-11.sha256"));
    const auto conn = slurp(data.path / "connectivity.csv");
    CHECK(z.at(f + "data/connectivity.csv") == conn.substr(0, conn.rfind('\n') + 1));

    CHECK(z.at(f + "report.html").starts_with("<!doctype html>"));
    CHECK(z.at(f + "verify.txt").find("Evidence trail in data, as of 2026-03-12 (UTC)\n") != std::string::npos);
    CHECK(z.at(f + "verify.txt").find("Result: OK.") != std::string::npos);
    const auto& readme = z.at(f + "README.txt");
    CHECK(readme.starts_with("meerkat export: 2026-03-10 to 2026-03-12 (UTC)\n"));
    CHECK(readme.find("by meerkat 0.0.0-test") != std::string::npos);
    CHECK(readme.find("Result: OK.") != std::string::npos);
    CHECK(readme.find("sha256sum -c SHA256SUMS") != std::string::npos);
    CHECK(readme.find("meerkat verify --data-dir data --as-of 2026-03-12\n") != std::string::npos);

    // SHA256SUMS: every other file, in `sha256sum -c` form.
    const auto& sums = z.at(f + "SHA256SUMS");
    std::size_t lines = 0;
    core::text::for_each_line(sums, [&](std::string_view line) {
        ++lines;
        const auto name = std::string{line.substr(66)};
        CAPTURE(name);
        REQUIRE(z.count(f + name) == 1);
        CHECK(line.substr(0, 64) == store::sha256_hex(z.at(f + name)));
        CHECK(line.substr(64, 2) == "  ");
    });
    CHECK(lines == 10);
}

TEST_CASE("meerkat export: the bundle passes its own check, later too (#95)") {
    test::TempDir data;
    test::TempDir outdir;
    fill(data.path, true);
    const auto zip_path = outdir.path / "b.zip";
    std::string out;
    std::string err;
    REQUIRE(run_export(config_for(data.path), {"2026-03-10", "2026-03-12", zip_path}, out, err) == 0);
    CHECK(out.find("Evidence trail: OK") != std::string::npos);
    // Sealed with the torn line: kept whole, old (03-10) or not (03-11).
    CHECK(out.find("raw/2026-03-10.jsonl") == std::string::npos);
    CHECK(out.find("raw/2026-03-11.jsonl") == std::string::npos);
    // Today's raw log isn't sealed: cut.
    CHECK(out.find("Left out the last 4 bytes of raw/2026-03-12.jsonl") != std::string::npos);

    // Unpack the data and check it as the provider would, weeks later.
    test::TempDir copy;
    const std::string f = "meerkat-export-2026-03-10-to-2026-03-12/data/";
    for (const auto& [name, content] : unzip(zip_path)) {
        if (name.starts_with(f)) {
            const auto p = copy.path / name.substr(f.size());
            fs::create_directories(p.parent_path());
            std::ofstream{p, std::ios::binary} << content;
        }
    }
    CHECK(slurp(copy.path / "raw" / "2026-03-10.jsonl") == slurp(data.path / "raw" / "2026-03-10.jsonl"));
    const auto later = at(24h * 60);
    std::ostringstream v_out;
    std::ostringstream v_err;
    CHECK(app::verify_command(copy.path, later, v_out, v_err, "2026-03-12") == 0);
    CHECK(v_out.str().find("Result: OK.") != std::string::npos);
    std::ostringstream v_out2;
    CHECK(app::verify_command(copy.path, later, v_out2, v_err) == 1);  // 03-12 never sealed: as of today a problem
}

TEST_CASE("meerkat export: an old unsealed raw log ends at its last complete line (#102)") {
    test::TempDir data;
    test::TempDir outdir;
    fill(data.path);
    append(data.path / "raw" / "2026-03-09.jsonl", "{\"time\":\"2026-03-09T23:00:00Z\"}\n{\"ti");
    std::string out;
    std::string err;
    REQUIRE(run_export(config_for(data.path), {"", "", outdir.path / "b.zip"}, out, err) == 0);
    CHECK(out.find("Left out the last 4 bytes of raw/2026-03-09.jsonl") != std::string::npos);
}

TEST_CASE("meerkat export: ZIP times are local wall-clock time (#95)") {
    test::TempDir data;
    test::TempDir outdir;
    fill(data.path);
    std::string out;
    std::string err;
    REQUIRE(run_export(config_for(data.path), {"", "", outdir.path / "b.zip"}, out, err, 1h) == 0);
    for (const auto& e : test::read_zip(slurp(outdir.path / "b.zip"))) {
        CAPTURE(e.name);
        CHECK(e.dos_time == (19 << 11));                         // 18:00 UTC, 19:00 local
        CHECK(e.dos_date == ((2026 - 1980) << 9 | 3 << 5 | 12));  // 2026-03-12
    }
}

TEST_CASE("meerkat export: problems in the trail are reported, the bundle still written") {
    test::TempDir data;
    test::TempDir outdir;
    fill(data.path);
    fs::remove(data.path / "raw" / "2026-03-10.jsonl");
    const auto zip_path = outdir.path / "b.zip";
    std::string out;
    std::string err;
    REQUIRE(run_export(config_for(data.path), {"", "", zip_path}, out, err) == 0);
    CHECK(out.find("Evidence trail: 2 problems; see verify.txt") != std::string::npos);
    const auto z = unzip(zip_path);
    const std::string f = "meerkat-export-2026-03-10-to-2026-03-12/";  // default range
    CHECK(z.at(f + "verify.txt").find("missing  raw/2026-03-10.jsonl") != std::string::npos);
    CHECK(z.at(f + "README.txt").find("Result: PROBLEMS, see the file.") != std::string::npos);
    CHECK(z.count(f + "data/raw/2026-03-10.jsonl") == 0);
}

TEST_CASE("meerkat export: the default file name, refusals and errors") {
    test::TempDir data;
    fill(data.path);
    std::string out;
    std::string err;

    SUBCASE("default name in the current directory") {
        test::TempDir cwd;
        const auto old = fs::current_path();
        fs::current_path(cwd.path);
        const int code = run_export(config_for(data.path), {"2026-03-11", "2026-03-12", {}}, out, err);
        fs::current_path(old);
        CHECK(code == 0);
        CHECK(fs::exists(cwd.path / "meerkat-export-2026-03-11-to-2026-03-12.zip"));
    }
    SUBCASE("not into the data directory") {
        CHECK(run_export(config_for(data.path), {"", "", data.path / "raw" / "x.zip"}, out, err) == 2);
        CHECK(err.find("won't write the export into the data directory") != std::string::npos);
        CHECK_FALSE(fs::exists(data.path / "raw" / "x.zip"));
    }
    SUBCASE("no speed.csv: said so (#90)") {
        test::TempDir outdir;
        fs::remove(data.path / "speed.csv");
        CHECK(run_export(config_for(data.path), {"2026-03-10", "2026-03-12", outdir.path / "x.zip"}, out, err) == 0);
        CHECK(out.find("No speed.csv in ") != std::string::npos);
    }
    SUBCASE("a raw log with no complete line") {
        test::TempDir outdir;
        append(data.path / "raw" / "2026-03-13.jsonl", "{\"time\"");
        CHECK(run_export(config_for(data.path), {"2026-03-10", "2026-03-12", outdir.path / "x.zip"}, out, err) == 0);
        CHECK(out.find("Left out the last 7 bytes of raw/2026-03-13.jsonl") != std::string::npos);
        const auto z = unzip(outdir.path / "x.zip");
        CHECK(z.at("meerkat-export-2026-03-10-to-2026-03-12/data/raw/2026-03-13.jsonl").empty());
    }
    SUBCASE("a bad date") {
        CHECK(run_export(config_for(data.path), {"2026-3-1", "", {}}, out, err) == 2);
        CHECK(err.find("--from must be a date") != std::string::npos);
    }
    SUBCASE("no data directory") {
        test::TempDir outdir;
        CHECK(run_export(config_for(data.path / "nope"), {"", "", outdir.path / "x.zip"}, out, err) == 1);
        CHECK(err.find("no data directory") != std::string::npos);
        CHECK(fs::is_empty(outdir.path));
    }
    SUBCASE("an output directory that doesn't exist") {
        test::TempDir outdir;
        CHECK(run_export(config_for(data.path), {"", "", outdir.path / "no" / "x.zip"}, out, err) == 1);
        CHECK(err.find("can't create") != std::string::npos);
    }
}
