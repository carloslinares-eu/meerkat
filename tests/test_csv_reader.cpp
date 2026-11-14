/**
 * @file test_csv_reader.cpp
 * @brief Tests for store::read_csv(): what CsvWriter writes reads back.
 */
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "store/csv.hpp"
#include "store/csv_reader.hpp"
#include "support.hpp"

using namespace meerkat::store;
namespace fs = std::filesystem;

namespace {

/// A two-column schema.
Schema schema() { return {.name = "test", .version = 1, .columns = {"time", "value"}}; }

/// Writes @p text as the whole file.
void put(const fs::path& p, const std::string& text) {
    std::ofstream{p, std::ios::binary | std::ios::trunc} << text;
}

/// Reads all rows.
std::vector<std::vector<std::string>> rows(const fs::path& p, CsvReadStats* stats = nullptr) {
    std::vector<std::vector<std::string>> out;
    const auto s = read_csv(p, schema(), [&out](const std::vector<std::string>& f) { out.push_back(f); });
    if (stats != nullptr) {
        *stats = s;
    }
    return out;
}

}  // namespace

TEST_CASE("read_csv reads back what CsvWriter wrote, quotes and line breaks included") {
    meerkat::test::TempDir dir;
    const auto file = dir.path / "t.csv";
    const std::vector<std::vector<std::string>> written{
        {"2026-11-11T20:00:00Z", "plain"},
        {"2026-11-11T20:05:00Z", ""},
        {"2026-11-11T20:10:00Z", "a,b"},
        {"2026-11-11T20:15:00Z", "say \"hi\""},
        {"2026-11-11T20:20:00Z", "two\r\nlines"},
        {"2026-11-11T20:25:00Z", "lone\rcr"},
    };
    {
        CsvWriter w{file, schema()};
        for (const auto& r : written) {
            w.write_row(r);
        }
    }
    CsvReadStats stats;
    CHECK(rows(file, &stats) == written);
    CHECK(stats.rows == written.size());
    CHECK(stats.skipped == 0);
}

TEST_CASE("read_csv skips torn rows and counts them") {
    meerkat::test::TempDir dir;
    const auto file = dir.path / "t.csv";
    const std::string h = header_text(schema());
    CsvReadStats stats;

    SUBCASE("a last row without its line end is still being written") {
        put(file, h + "t1,1\r\nt2,2");
        CHECK(rows(file, &stats) == std::vector<std::vector<std::string>>{{"t1", "1"}});
        CHECK(stats.skipped == 1);
    }
    SUBCASE("a torn row repaired by CsvWriter has too few fields") {
        put(file, h + "t1\r\nt2,2\r\n");  // a row cut after its first field
        CHECK(rows(file, &stats) == std::vector<std::vector<std::string>>{{"t2", "2"}});
        CHECK(stats.skipped == 1);
    }
    SUBCASE("an open quote closed by CsvWriter swallows the line break: one short row") {
        put(file, h + "t1,\"cut\r\nmore\"\r\nt2,2\r\n");
        // "t1" and the quoted field: two fields, so this one is kept as a row
        // with the break inside; the next row is fine.
        const auto r = rows(file, &stats);
        REQUIRE(r.size() == 2);
        CHECK(r[0][1] == "cut\r\nmore");
        CHECK(r[1] == std::vector<std::string>{"t2", "2"});
    }
    SUBCASE("bare LF line ends are accepted") {
        put(file, "# meerkat test v1\ntime,value\nt1,1\n");
        CHECK(rows(file) == std::vector<std::vector<std::string>>{{"t1", "1"}});
    }
    SUBCASE("an empty file has no rows") {
        put(file, "");
        CHECK(rows(file, &stats).empty());
        CHECK(stats.skipped == 0);
    }
}

TEST_CASE("read_csv refuses another schema and reports a missing file") {
    meerkat::test::TempDir dir;
    const auto file = dir.path / "t.csv";
    put(file, "# meerkat other v1\r\ntime,value\r\n");
    CHECK_THROWS_AS(rows(file), SchemaMismatch);
    put(file, "# meerkat test v1\r\ntime,speed\r\n");
    CHECK_THROWS_AS(rows(file), SchemaMismatch);
    put(file, "# meerkat test v1\r\n");
    CHECK_THROWS_AS(rows(file), SchemaMismatch);
    try {
        rows(dir.path / "missing.csv");
        FAIL("no error");
    } catch (const std::system_error& e) {
        CHECK(e.code() == std::errc::no_such_file_or_directory);
    }
}
