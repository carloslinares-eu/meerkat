/**
 * @file test_manifest.cpp
 * @brief Tests for the daily SHA-256 manifest.
 */
#include <doctest/doctest.h>

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "store/manifest.hpp"
#include "store/sha256.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace meerkat::store;
using meerkat::test::TempDir;
namespace fs = std::filesystem;

namespace {

/// 2026-11-12T00:00:05Z.
core::Timestamp created() {
    using namespace std::chrono;
    return sys_days{year{2026} / 11 / 12} + seconds{5};
}

/// Writes @p text to @p p, creating the folders.
void put(const fs::path& p, const std::string& text, bool append = false) {
    fs::create_directories(p.parent_path());
    std::ofstream out{p, std::ios::binary | (append ? std::ios::app : std::ios::trunc)};
    out << text;
}

/// A data directory with a raw log for 2026-11-11 and both CSVs.
void fill(const fs::path& data) {
    put(data / "raw" / "2026-11-11.jsonl", "{\"time\":\"2026-11-11T20:00:00Z\"}\n");
    put(data / "speed.csv", "# meerkat speed v1\r\ntime\r\n2026-11-11T20:00:00Z\r\n");
    put(data / "connectivity.csv", "# meerkat connectivity v1\r\ntime\r\n");
}

/// Outcomes of verify_manifest(), in order.
std::vector<EntryCheck::Outcome> outcomes(const fs::path& data, const Manifest& m) {
    std::vector<EntryCheck::Outcome> v;
    for (const auto& c : verify_manifest(data, m)) {
        v.push_back(c.outcome);
    }
    return v;
}

using O = EntryCheck::Outcome;

}  // namespace

TEST_CASE("valid_day") {
    CHECK(valid_day("2026-11-11"));
    CHECK(valid_day("2028-02-29"));
    CHECK_FALSE(valid_day("2026-02-29"));
    CHECK_FALSE(valid_day("2026-13-01"));
    CHECK_FALSE(valid_day("2026-11-1"));
    CHECK_FALSE(valid_day("2026/11/11"));
    CHECK_FALSE(valid_day("2026-11-11T"));
    CHECK_FALSE(valid_day(""));
}

TEST_CASE("daily_files and manifest_path") {
    CHECK(daily_files("2026-11-11") ==
          std::vector<std::string>{"raw/2026-11-11.jsonl", "speed.csv", "connectivity.csv"});
    CHECK(manifest_path("/data", "2026-11-11") == fs::path{"/data/manifest/2026-11-11.sha256"});
}

TEST_CASE("build_manifest hashes the day's files and skips missing ones") {
    TempDir dir;
    fill(dir.path);
    const auto m = build_manifest(dir.path, "2026-11-11", created(), daily_files("2026-11-11"));
    CHECK(m.day == "2026-11-11");
    CHECK(m.created == created());
    REQUIRE(m.entries.size() == 3);
    CHECK(m.entries[0].path == "raw/2026-11-11.jsonl");
    CHECK(m.entries[0].sha256 == sha256_hex("{\"time\":\"2026-11-11T20:00:00Z\"}\n"));
    CHECK(m.entries[0].size == 32);
    CHECK(m.entries[1].path == "speed.csv");
    CHECK(m.entries[2].path == "connectivity.csv");

    SUBCASE("a day without speed tests has no raw log") {
        const auto m2 = build_manifest(dir.path, "2026-11-12", created(), daily_files("2026-11-12"));
        REQUIRE(m2.entries.size() == 2);
        CHECK(m2.entries[0].path == "speed.csv");
    }
    SUBCASE("bad input") {
        CHECK_THROWS_AS(build_manifest(dir.path, "2026-11-32", created(), {}), std::invalid_argument);
        CHECK_THROWS_AS(build_manifest(dir.path, "2026-11-11", created(), {"../etc/passwd"}),
                        std::invalid_argument);
        CHECK_THROWS_AS(build_manifest(dir.path, "2026-11-11", created(), {"/etc/passwd"}),
                        std::invalid_argument);
    }
}

TEST_CASE("format_manifest and parse_manifest round-trip") {
    Manifest m;
    m.day = "2026-11-11";
    m.created = created();
    m.entries.push_back({std::string(64, 'a'), 123, "raw/2026-11-11.jsonl"});
    m.entries.push_back({std::string(64, '0'), 0, "speed.csv"});
    const auto text = format_manifest(m);
    CHECK(text ==
          "# meerkat manifest v1\n"
          "# day 2026-11-11\n"
          "# created 2026-11-12T00:00:05Z\n" +
              std::string(64, 'a') + "  123  raw/2026-11-11.jsonl\n" + std::string(64, '0') +
              "  0  speed.csv\n");

    const auto back = parse_manifest(text);
    REQUIRE(back.has_value());
    CHECK(back->day == m.day);
    CHECK(back->created == m.created);
    REQUIRE(back->entries.size() == 2);
    CHECK(back->entries[0].sha256 == m.entries[0].sha256);
    CHECK(back->entries[0].size == 123);
    CHECK(back->entries[1].path == "speed.csv");

    SUBCASE("a manifest without entries is valid") {
        Manifest empty;
        empty.day = "2026-11-11";
        empty.created = created();
        CHECK(parse_manifest(format_manifest(empty)).has_value());
    }
    SUBCASE("invalid manifests are rejected") {
        const std::string head = "# meerkat manifest v1\n# day 2026-11-11\n# created 2026-11-12T00:00:05Z\n";
        const std::string hash(64, 'a');
        CHECK_FALSE(parse_manifest("").has_value());
        CHECK_FALSE(parse_manifest("# meerkat manifest v2\n# day 2026-11-11\n# created 2026-11-12T00:00:05Z\n"));
        CHECK_FALSE(parse_manifest("# meerkat manifest v1\n# day 2026-11-31\n# created 2026-11-12T00:00:05Z\n"));
        CHECK_FALSE(parse_manifest("# meerkat manifest v1\n# day 2026-11-11\n# created yesterday\n"));
        CHECK_FALSE(parse_manifest(head + hash + "  1  speed.csv").has_value());  // no final newline
        CHECK_FALSE(parse_manifest(head + std::string(63, 'a') + "  1  speed.csv\n").has_value());
        CHECK_FALSE(parse_manifest(head + std::string(64, 'A') + "  1  speed.csv\n").has_value());
        CHECK_FALSE(parse_manifest(head + hash + "  -1  speed.csv\n").has_value());
        CHECK_FALSE(parse_manifest(head + hash + " 1 speed.csv\n").has_value());
        CHECK_FALSE(parse_manifest(head + hash + "  1  ../speed.csv\n").has_value());
        CHECK_FALSE(parse_manifest(head + hash + "  1  /etc/passwd\n").has_value());
        CHECK_FALSE(parse_manifest(head + hash + "  1  \n").has_value());
    }
}

TEST_CASE("write_manifest writes once, read-only, and verify_manifest detects edits") {
    TempDir dir;
    fill(dir.path);
    const auto m = build_manifest(dir.path, "2026-11-11", created(), daily_files("2026-11-11"));
    const auto path = write_manifest(dir.path, m);
    CHECK(path == manifest_path(dir.path, "2026-11-11"));
    CHECK(core::text::read_file(path) == format_manifest(m));
    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0440);
    CHECK_FALSE(fs::exists(dir.path / "manifest" / ".2026-11-11.sha256.tmp"));

    try {
        write_manifest(dir.path, m);
        FAIL("overwrote an existing manifest");
    } catch (const std::system_error& e) {
        CHECK(e.code() == std::errc::file_exists);
    }

    CHECK(outcomes(dir.path, m) == std::vector<O>{O::ok, O::ok, O::ok});

    SUBCASE("new rows appended later don't break the check") {
        put(dir.path / "speed.csv", "2026-11-12T00:05:00Z\r\n", true);
        CHECK(outcomes(dir.path, m) == std::vector<O>{O::ok, O::ok, O::ok});
    }
    SUBCASE("an edited old row is found") {
        put(dir.path / "speed.csv", "# meerkat speed v1\r\ntime\r\n2026-11-11T20:00:01Z\r\n");
        CHECK(outcomes(dir.path, m) == std::vector<O>{O::ok, O::changed, O::ok});
    }
    SUBCASE("removed rows are found") {
        put(dir.path / "speed.csv", "# meerkat speed v1\r\ntime\r\n");
        CHECK(outcomes(dir.path, m) == std::vector<O>{O::ok, O::shorter, O::ok});
    }
    SUBCASE("a deleted file is found") {
        fs::remove(dir.path / "raw" / "2026-11-11.jsonl");
        CHECK(outcomes(dir.path, m) == std::vector<O>{O::missing, O::ok, O::ok});
    }
}

TEST_CASE("each manifest includes the latest earlier one (the chain)") {
    TempDir dir;
    fill(dir.path);
    // Nothing earlier: no chain entry.
    const auto m1 = build_manifest(dir.path, "2026-11-11", created(), daily_files("2026-11-11"));
    CHECK(m1.entries.front().path == "raw/2026-11-11.jsonl");
    write_manifest(dir.path, m1);

    // A gap (no manifest for the 12th): the 13th chains to the 11th.
    put(dir.path / "manifest" / ".2026-11-12.sha256.tmp", "leftover");  // ignored
    put(dir.path / "manifest" / "notes.txt", "ignored");
    const auto m3 = build_manifest(dir.path, "2026-11-13", created(), daily_files("2026-11-13"));
    REQUIRE_FALSE(m3.entries.empty());
    CHECK(m3.entries.front().path == "manifest/2026-11-11.sha256");
    CHECK(m3.entries.front().sha256 == sha256_hex(format_manifest(m1)));
    write_manifest(dir.path, m3);

    // A manifest for an earlier day doesn't chain to a later one.
    const auto m0 = build_manifest(dir.path, "2026-11-10", created(), {});
    CHECK(m0.entries.empty());

    // Replacing the 11th's manifest breaks the 13th's check.
    fs::permissions(manifest_path(dir.path, "2026-11-11"), fs::perms::owner_write,
                    fs::perm_options::add);
    put(manifest_path(dir.path, "2026-11-11"), format_manifest(m0));
    CHECK(verify_manifest(dir.path, m3).front().outcome != O::ok);
}
