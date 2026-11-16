/**
 * @file test_verify.cpp
 * @brief Tests for the evidence-trail check (`meerkat verify`): a data
 *        directory sealed by the real manifest code, then edited.
 */
#include <doctest/doctest.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app/verify.hpp"
#include "store/manifest.hpp"
#include "store/raw_log.hpp"
#include "store/verify.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace meerkat::store;
using meerkat::test::TempDir;
namespace fs = std::filesystem;
using std::chrono::days;
using std::chrono::sys_days;

namespace {

/// @return The day @p d of November 2026.
sys_days nov(unsigned d) { return sys_days{std::chrono::year{2026} / std::chrono::month{11} / std::chrono::day{d}}; }

/// Writes @p text to @p p, creating the folders.
void put(const fs::path& p, const std::string& text, bool append = false) {
    fs::create_directories(p.parent_path());
    std::ofstream out{p, std::ios::binary | (append ? std::ios::app : std::ios::trunc)};
    out << text;
}

/// Appends one day of data: a raw-log line and a row in each CSV.
void record(const fs::path& data, sys_days d) {
    const auto day = utc_day(core::Timestamp{d});
    put(data / "raw" / (day + ".jsonl"), "{\"time\":\"" + day + "T12:00:00Z\"}\n", true);
    put(data / "speed.csv", day + "T12:00:00Z,ok\r\n", true);
    put(data / "connectivity.csv", day + "T12:00:30Z,ok\r\n", true);
}

/// Seals @p d as `meerkat run` does, just after it ended.
void seal(const fs::path& data, sys_days d) {
    const auto day = utc_day(core::Timestamp{d});
    write_manifest(data, build_manifest(data, day, core::Timestamp{d + days{1}} + std::chrono::seconds{5},
                                        daily_files(day)));
}

/// A data directory with data for 10 to 12 November, each day sealed.
void three_days(const fs::path& data) {
    put(data / "speed.csv", "# meerkat speed v1\r\ntime,status\r\n");
    put(data / "connectivity.csv", "# meerkat connectivity v1\r\ntime,status\r\n");
    for (unsigned d = 10; d <= 12; ++d) {
        record(data, nov(d));
        seal(data, nov(d));
    }
}

/// @return Whether a finding of @p level contains @p text.
bool has(const VerifyResult& r, VerifyFinding::Level level, const std::string& text) {
    return std::any_of(r.findings.begin(), r.findings.end(), [&](const VerifyFinding& f) {
        return f.level == level && f.text.find(text) != std::string::npos;
    });
}

/// @return The outcome of @p path in the manifest of @p day.
std::optional<EntryCheck::Outcome> outcome(const VerifyResult& r, const std::string& day,
                                           const std::string& path) {
    for (const auto& m : r.manifests) {
        for (const auto& e : m.entries) {
            if (m.day == day && e.entry.path == path) {
                return e.outcome;
            }
        }
    }
    return std::nullopt;
}

/// Overwrites one byte of @p p at @p offset, keeping the size.
void flip(const fs::path& p, std::streamoff offset) {
    fs::permissions(p, fs::perms::owner_write, fs::perm_options::add);
    std::fstream f{p, std::ios::in | std::ios::out | std::ios::binary};
    f.seekp(offset);
    f.put('X');
}

using P = VerifyFinding::Level;
using O = EntryCheck::Outcome;

}  // namespace

TEST_CASE("verify_data: an untouched trail is ok") {
    TempDir dir;
    three_days(dir.path);
    const auto r = verify_data(dir.path, nov(13));
    CHECK(r.ok());
    CHECK(r.problems() == 0);
    REQUIRE(r.manifests.size() == 3);
    CHECK(r.manifests[0].day == "2026-11-10");
    CHECK(r.manifests[2].manifest.has_value());
    CHECK(r.manifests[2].sha256.size() == 64);
    CHECK(outcome(r, "2026-11-12", "manifest/2026-11-11.sha256") == O::ok);
    CHECK(outcome(r, "2026-11-11", "raw/2026-11-10.jsonl") == O::ok);
    CHECK(r.findings.empty());

    // Today's data isn't sealed yet: a note, not a problem.
    record(dir.path, nov(13));
    const auto later = verify_data(dir.path, nov(13));
    CHECK(later.ok());
    CHECK(has(later, P::note, "speed.csv: "));
    CHECK(has(later, P::note, "connectivity.csv: "));
}

TEST_CASE("verify_data: edits, removals and additions to sealed files") {
    TempDir dir;
    three_days(dir.path);
    const auto& data = dir.path;

    SUBCASE("a changed CSV row") {
        flip(data / "speed.csv", 40);
        const auto r = verify_data(data, nov(13));
        CHECK_FALSE(r.ok());
        CHECK(outcome(r, "2026-11-12", "speed.csv") == O::changed);
        CHECK(outcome(r, "2026-11-10", "speed.csv") == O::changed);
    }
    SUBCASE("a shortened CSV") {
        fs::resize_file(data / "connectivity.csv", 50);
        const auto r = verify_data(data, nov(13));
        CHECK(outcome(r, "2026-11-12", "connectivity.csv") == O::shorter);
    }
    SUBCASE("a line added to a raw log after the next day sealed it") {
        put(data / "raw" / "2026-11-10.jsonl", "{\"time\":\"2026-11-10T13:00:00Z\"}\n", true);
        const auto r = verify_data(data, nov(13));
        CHECK(outcome(r, "2026-11-11", "raw/2026-11-10.jsonl") == O::grew);
        CHECK(outcome(r, "2026-11-10", "raw/2026-11-10.jsonl") == O::ok);  // its own day: a prefix
        CHECK(r.problems() == 1);
    }
    SUBCASE("a removed raw log") {
        fs::remove(data / "raw" / "2026-11-11.jsonl");
        const auto r = verify_data(data, nov(13));
        CHECK(outcome(r, "2026-11-11", "raw/2026-11-11.jsonl") == O::missing);
        CHECK(outcome(r, "2026-11-12", "raw/2026-11-11.jsonl") == O::missing);
    }
    SUBCASE("a raw log created after the next day was sealed") {
        fs::remove(data / "raw" / "2026-11-11.jsonl");
        fs::remove(data / "manifest" / "2026-11-12.sha256");
        fs::remove(data / "manifest" / "2026-11-11.sha256");
        seal(data, nov(11));
        seal(data, nov(12));
        put(data / "raw" / "2026-11-11.jsonl", "{\"time\":\"2026-11-11T12:00:00Z\"}\n");
        const auto r = verify_data(data, nov(13));
        CHECK(has(r, P::problem, "raw/2026-11-11.jsonl isn't in the manifest of 2026-11-12"));
    }
}

TEST_CASE("verify_data: the chain") {
    TempDir dir;
    three_days(dir.path);
    const auto& data = dir.path;
    const auto mdir = data / "manifest";

    SUBCASE("a removed manifest in the middle") {
        fs::remove(mdir / "2026-11-11.sha256");
        const auto r = verify_data(data, nov(13));
        CHECK(outcome(r, "2026-11-12", "manifest/2026-11-11.sha256") == O::missing);
        CHECK(has(r, P::problem, "no manifest for 2026-11-11 (1 day), between sealed days"));
        CHECK(has(r, P::problem, "lists manifest/2026-11-11.sha256, but the manifest before it is "
                                 "manifest/2026-11-10.sha256"));
    }
    SUBCASE("a removed first manifest") {
        fs::remove(mdir / "2026-11-10.sha256");
        const auto r = verify_data(data, nov(13));
        CHECK(outcome(r, "2026-11-11", "manifest/2026-11-10.sha256") == O::missing);
        CHECK(has(r, P::note, "there is no earlier manifest: the start of the chain was removed"));
        CHECK(r.problems() == 2);  // the missing entry, and the day before the first manifest
        CHECK(has(r, P::problem, "2026-11-10 (1 day) have data but no manifest, before the first one"));
    }
    SUBCASE("a rewritten manifest") {
        // Re-sealed after editing the data: its own entries match, but the
        // next manifest has the old one's hash.
        flip(data / "raw" / "2026-11-11.jsonl", 3);
        fs::permissions(mdir / "2026-11-11.sha256", fs::perms::owner_write, fs::perm_options::add);
        fs::remove(mdir / "2026-11-11.sha256");
        seal(data, nov(11));
        const auto r = verify_data(data, nov(13));
        CHECK(outcome(r, "2026-11-11", "raw/2026-11-11.jsonl") == O::ok);
        CHECK(outcome(r, "2026-11-12", "manifest/2026-11-11.sha256") == O::changed);
        CHECK(outcome(r, "2026-11-12", "raw/2026-11-11.jsonl") == O::changed);
    }
    SUBCASE("a manifest without the chain entry") {
        fs::permissions(mdir / "2026-11-12.sha256", fs::perms::owner_write, fs::perm_options::add);
        std::string text;
        {
            std::ifstream in{mdir / "2026-11-12.sha256"};
            std::stringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        const auto at = text.find("  manifest/");
        const auto start = text.rfind('\n', at) + 1;
        text.erase(start, text.find('\n', at) + 1 - start);
        put(mdir / "2026-11-12.sha256", text);
        const auto r = verify_data(data, nov(13));
        CHECK(has(r, P::problem, "doesn't list the manifest before it (manifest/2026-11-11.sha256)"));
    }
    SUBCASE("a manifest that isn't one") {
        fs::permissions(mdir / "2026-11-12.sha256", fs::perms::owner_write, fs::perm_options::add);
        put(mdir / "2026-11-12.sha256", "hello\n");
        const auto r = verify_data(data, nov(13));
        CHECK(has(r, P::problem, "manifest/2026-11-12.sha256 isn't a valid meerkat manifest"));
        CHECK_FALSE(r.manifests[2].manifest.has_value());
        CHECK(r.manifests[2].sha256.size() == 64);
    }
    SUBCASE("a manifest under another day's name") {
        fs::rename(mdir / "2026-11-12.sha256", mdir / "2026-11-13.sha256");
        const auto r = verify_data(data, nov(14));
        CHECK(has(r, P::problem, "manifest/2026-11-13.sha256 says it is the manifest of 2026-11-12"));
        CHECK(has(r, P::problem, "no manifest for 2026-11-12 (1 day)"));
    }
    SUBCASE("a symlink, a stray file and a temp file") {
        fs::create_symlink("2026-11-12.sha256", mdir / "2026-11-13.sha256");
        put(mdir / "README", "notes\n");
        put(mdir / ".2026-11-14.sha256.123.tmp", "half");
        const auto r = verify_data(data, nov(14));
        CHECK(has(r, P::problem, "manifest/2026-11-13.sha256 isn't a regular file"));
        CHECK(has(r, P::note, "manifest/README isn't a manifest; ignored"));
        CHECK_FALSE(has(r, P::note, ".tmp"));
        CHECK(r.manifests.size() == 3);
    }
}

TEST_CASE("verify_data: days without a manifest") {
    TempDir dir;
    three_days(dir.path);

    SUBCASE("yesterday is a note, earlier days are a problem") {
        auto r = verify_data(dir.path, nov(14));
        CHECK(r.ok());
        CHECK(has(r, P::note, "yesterday (2026-11-13, UTC) isn't sealed yet"));
        r = verify_data(dir.path, nov(17));
        CHECK_FALSE(r.ok());
        CHECK(has(r, P::problem, "2026-11-13 to 2026-11-15 (3 days) aren't sealed"));
        CHECK(has(r, P::note, "yesterday (2026-11-16, UTC)"));
    }
    SUBCASE("a raw log dated in the future") {
        record(dir.path, nov(20));
        const auto r = verify_data(dir.path, nov(13));
        CHECK(has(r, P::note, "raw/2026-11-20.jsonl is dated after today"));
    }
    SUBCASE("data and no manifest at all") {
        fs::remove_all(dir.path / "manifest");
        const auto r = verify_data(dir.path, nov(13));
        CHECK(r.manifests.empty());
        CHECK(has(r, P::problem, "2026-11-10 to 2026-11-11 (2 days) aren't sealed"));
        CHECK(has(r, P::note, "yesterday (2026-11-12, UTC)"));
    }
}

TEST_CASE("verify_data: the 31-day backlog, future manifests, skipped days") {
    TempDir dir;
    three_days(dir.path);
    const auto& data = dir.path;

    SUBCASE("days older than the backlog can't be sealed any more") {
        const auto today = nov(12) + days{40};  // 2026-12-22
        const auto r = verify_data(data, today);
        CHECK(has(r, P::problem, "2026-11-13 to 2026-11-20 (8 days) aren't sealed, and can't be any more"));
        CHECK(has(r, P::problem, "2026-11-21 to 2026-12-20 (30 days) aren't sealed: start `meerkat run`"));
        CHECK(has(r, P::note, "yesterday (2026-12-21, UTC)"));
    }
    SUBCASE("a manifest dated today or later") {
        record(data, nov(30));
        seal(data, nov(30));
        const auto r = verify_data(data, nov(16));
        CHECK(has(r, P::problem, "manifest/2026-11-30.sha256 is dated today or later"));
        CHECK(has(r, P::problem, "2026-11-13 to 2026-11-14 (2 days) aren't sealed"));
        CHECK(has(r, P::note, "yesterday (2026-11-15, UTC)"));
    }
    SUBCASE("unsealed days before the first data that's left") {
        // Days after the last manifest count from there, even if the data
        // of those days is gone.
        for (const auto* f : {"speed.csv", "connectivity.csv", "raw/2026-11-10.jsonl",
                              "raw/2026-11-11.jsonl", "raw/2026-11-12.jsonl"}) {
            fs::remove(data / f);
        }
        record(data, nov(14));  // the first data day is now 2026-11-14
        const auto r = verify_data(data, nov(16));
        CHECK(has(r, P::problem, "2026-11-13 to 2026-11-14 (2 days) aren't sealed"));
    }
    SUBCASE("a raw log created after its own day's manifest") {
        fs::remove(data / "raw" / "2026-11-12.jsonl");
        fs::remove(data / "manifest" / "2026-11-12.sha256");
        seal(data, nov(12));  // now without raw/2026-11-12.jsonl
        put(data / "raw" / "2026-11-12.jsonl", "{\"time\":\"2026-11-12T23:59:50Z\"}\n");
        const auto r = verify_data(data, nov(13));
        CHECK(has(r, P::note, "raw/2026-11-12.jsonl was created after the manifest of 2026-11-12"));
    }
}

TEST_CASE("verify_text: a day with a problem isn't shown as ok") {
    TempDir dir;
    three_days(dir.path);
    const auto mdir = dir.path / "manifest";
    fs::rename(mdir / "2026-11-10.sha256", mdir / "2026-11-09.sha256");
    const auto r = verify_data(dir.path, nov(13));
    const auto text = app::verify_text(r, dir.path);
    CHECK(text.find("2026-11-09  PROBLEM") != std::string::npos);
    CHECK(has(r, P::problem, "says it is the manifest of 2026-11-10"));
}

TEST_CASE("verify_data: an empty data directory") {
    TempDir dir;
    const auto r = verify_data(dir.path, nov(13));
    CHECK(r.ok());
    CHECK(has(r, P::note, "no data and no manifests yet"));
}

TEST_CASE("verify_command") {
    TempDir dir;
    three_days(dir.path);
    const auto now = core::Timestamp{nov(13)} + std::chrono::hours{9};

    std::ostringstream out;
    std::ostringstream err;
    CHECK(app::verify_command(dir.path, now, out, err) == 0);
    const auto text = out.str();
    CHECK(text.find("2026-11-12  ok  manifest sha256 ") != std::string::npos);
    CHECK(text.find("    ok       raw/2026-11-11.jsonl (") != std::string::npos);
    CHECK(text.find("Result: OK. 3 manifests, 13 files checked") != std::string::npos);
    CHECK(err.str().empty());

    flip(dir.path / "speed.csv", 40);
    std::ostringstream out2;
    CHECK(app::verify_command(dir.path, now, out2, err) == 1);
    CHECK(out2.str().find("2026-11-12  PROBLEM") != std::string::npos);
    CHECK(out2.str().find("changed  speed.csv (") != std::string::npos);
    CHECK(out2.str().find(": the sealed part was edited") != std::string::npos);
    CHECK(out2.str().find("Result: 3 problems in 3 manifests and 13 files.") != std::string::npos);

    std::ostringstream out3;
    std::ostringstream err3;
    CHECK(app::verify_command(dir.path / "nope", now, out3, err3) == 1);
    CHECK(err3.str().find("no data directory") != std::string::npos);
}
