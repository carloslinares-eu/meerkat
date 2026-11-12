/**
 * @file test_csv.cpp
 * @brief Tests for store::CsvWriter and the CSV escaping.
 */
#include <doctest/doctest.h>

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/text.hpp"
#include "store/csv.hpp"
#include "support.hpp"

using namespace meerkat::store;
namespace fs = std::filesystem;
using meerkat::test::TempDir;

namespace {

/// A two-column schema for the tests.
Schema test_schema(int version = 1) { return {"test", version, {"time", "value"}}; }

/// The whole file.
std::string slurp(const fs::path& p) { return meerkat::core::text::read_file(p).value(); }

/// Overwrites @p p with @p text.
void put(const fs::path& p, const std::string& text) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
}

}  // namespace

TEST_CASE("escape_field follows RFC 4180") {
    CHECK(escape_field("") == "");
    CHECK(escape_field("plain") == "plain");
    CHECK(escape_field("93.512") == "93.512");
    CHECK(escape_field("-58") == "-58");
    CHECK(escape_field("a,b") == "\"a,b\"");
    CHECK(escape_field("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(escape_field("\"") == "\"\"\"\"");
    CHECK(escape_field("two\nlines") == "\"two\nlines\"");
    CHECK(escape_field("cr\rhere") == "\"cr\rhere\"");
    CHECK(escape_field(" padded") == "\" padded\"");
    CHECK(escape_field("padded ") == "\"padded \"");
    CHECK(escape_field("in the middle") == "in the middle");
    CHECK(escape_field("Example ISP, Inc.") == "\"Example ISP, Inc.\"");
}

TEST_CASE("format_record and header_text") {
    CHECK(format_record({"a", "", "c,d"}) == "a,,\"c,d\"\r\n");
    CHECK(format_record({""}) == "\r\n");
    CHECK(header_text(test_schema(3)) == "# meerkat test v3\r\ntime,value\r\n");
}

TEST_CASE("CsvWriter creates a file with the header and appends rows") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    {
        CsvWriter w{file, test_schema()};
        CHECK(w.path() == file);
        CHECK(w.schema().name == "test");
        CHECK(slurp(file) == "# meerkat test v1\r\ntime,value\r\n");
        w.write_row({"2026-11-11T20:00:00Z", "1.5"});
        w.write_row({"2026-11-11T20:05:00Z", ""});
    }
    CHECK(slurp(file) ==
          "# meerkat test v1\r\ntime,value\r\n"
          "2026-11-11T20:00:00Z,1.5\r\n"
          "2026-11-11T20:05:00Z,\r\n");

    struct stat st {};
    REQUIRE(::stat(file.c_str(), &st) == 0);
    CHECK((st.st_mode & 0007) == 0);  // never readable by others

    SUBCASE("reopening appends below the existing rows") {
        CsvWriter w{file, test_schema()};
        w.write_row({"2026-11-11T20:10:00Z", "a \"quoted\", value"});
        CHECK(slurp(file) ==
              "# meerkat test v1\r\ntime,value\r\n"
              "2026-11-11T20:00:00Z,1.5\r\n"
              "2026-11-11T20:05:00Z,\r\n"
              "2026-11-11T20:10:00Z,\"a \"\"quoted\"\", value\"\r\n");
    }
}

TEST_CASE("CsvWriter writes the header into an existing empty file") {
    TempDir dir;
    const auto file = dir.path / "empty.csv";
    put(file, "");
    CsvWriter w{file, test_schema()};
    CHECK(slurp(file) == header_text(test_schema()));
}

TEST_CASE("CsvWriter refuses a file with a different schema and leaves it alone") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    { CsvWriter w{file, test_schema()}; }
    const std::string before = slurp(file);

    SUBCASE("other version") { CHECK_THROWS_AS(CsvWriter(file, test_schema(2)), SchemaMismatch); }
    SUBCASE("other name") {
        CHECK_THROWS_AS(CsvWriter(file, Schema{"other", 1, {"time", "value"}}), SchemaMismatch);
    }
    SUBCASE("extra column") {
        CHECK_THROWS_AS(CsvWriter(file, Schema{"test", 1, {"time", "value", "more"}}),
                        SchemaMismatch);
    }
    SUBCASE("fewer columns") {
        CHECK_THROWS_AS(CsvWriter(file, Schema{"test", 1, {"time"}}), SchemaMismatch);
    }
    SUBCASE("renamed column") {
        CHECK_THROWS_AS(CsvWriter(file, Schema{"test", 1, {"time", "valu"}}), SchemaMismatch);
    }
    CHECK(slurp(file) == before);
}

TEST_CASE("CsvWriter refuses files that aren't meerkat CSVs") {
    TempDir dir;
    const auto file = dir.path / "foreign.csv";

    SUBCASE("no version line") {
        put(file, "time,value\r\n1,2\r\n");
        CHECK_THROWS_AS(CsvWriter(file, test_schema()), SchemaMismatch);
        CHECK(slurp(file) == "time,value\r\n1,2\r\n");
    }
    SUBCASE("LF line endings") {
        put(file, "# meerkat test v1\ntime,value\n");
        CHECK_THROWS_AS(CsvWriter(file, test_schema()), SchemaMismatch);
    }
    SUBCASE("shorter than the header and not a piece of it") {
        put(file, "# meerkat tx");  // not the start of our header
        CHECK_THROWS_AS(CsvWriter(file, test_schema()), SchemaMismatch);
    }
    SUBCASE("message names what was found") {
        put(file, "# meerkat test v7\r\ntime,value\r\n");
        try {
            CsvWriter w{file, test_schema()};
            FAIL("no exception");
        } catch (const SchemaMismatch& e) {
            CHECK(std::string{e.what()}.find("found '# meerkat test v7'") != std::string::npos);
        }
    }
}

TEST_CASE("CsvWriter ends a torn last row before appending") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    put(file, header_text(test_schema()) + "2026-11-11T20:00:00Z,1.");
    CsvWriter w{file, test_schema()};
    w.write_row({"2026-11-11T20:05:00Z", "2"});
    CHECK(slurp(file) == header_text(test_schema()) +
                             "2026-11-11T20:00:00Z,1.\r\n"
                             "2026-11-11T20:05:00Z,2\r\n");
}

TEST_CASE("CsvWriter closes an open quoted field left by a crash") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    const auto h = header_text(test_schema());

    SUBCASE("quote still open") {
        put(file, h + "t1,\"curl: (28) timeout\r\nretry");
        CsvWriter w{file, test_schema()};
        w.write_row({"t2", "v"});
        CHECK(slurp(file) == h + "t1,\"curl: (28) timeout\r\nretry\"\r\nt2,v\r\n");
    }
    SUBCASE("ends in an escaped quote, still open") {
        put(file, h + "t1,\"say \"\"");
        CsvWriter w{file, test_schema()};
        CHECK(slurp(file) == h + "t1,\"say \"\"\"\r\n");
    }
    SUBCASE("a complete row with a quoted line break needs no repair") {
        const auto body = h + "t1,\"two\r\nlines\"\r\n";
        put(file, body);
        CsvWriter w{file, test_schema()};
        CHECK(slurp(file) == body);
    }
    SUBCASE("a closed quote at the very end only needs the line break") {
        put(file, h + "t1,\"a,b\"");
        CsvWriter w{file, test_schema()};
        CHECK(slurp(file) == h + "t1,\"a,b\"\r\n");
    }
}

TEST_CASE("CsvWriter completes a CRLF cut after the CR") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    put(file, header_text(test_schema()) + "t1,v\r");
    CsvWriter w{file, test_schema()};
    CHECK(slurp(file) == header_text(test_schema()) + "t1,v\r\n");
}

TEST_CASE("CsvWriter rewrites a header cut short by a crash") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    const auto h = header_text(test_schema());
    for (const std::size_t n : {std::size_t{1}, std::size_t{12}, h.size() - 1}) {
        CAPTURE(n);
        put(file, h.substr(0, n));
        { CsvWriter w{file, test_schema()}; }
        CHECK(slurp(file) == h);
    }
}

TEST_CASE("a failed write closes the writer; the next one repairs the row") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    const auto h = header_text(test_schema());
    CsvWriter w{file, test_schema()};

    // Let the file grow by only 4 more bytes: the row is cut off, then write()
    // fails with EFBIG (SIGXFSZ is ignored for the duration).
    rlimit old{};
    REQUIRE(::getrlimit(RLIMIT_FSIZE, &old) == 0);
    const auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
    rlimit small = old;
    small.rlim_cur = h.size() + 4;
    REQUIRE(::setrlimit(RLIMIT_FSIZE, &small) == 0);
    bool threw = false;
    try {
        w.write_row({"2026-11-11T20:00:00Z", "1.5"});
    } catch (const std::system_error&) {
        threw = true;
    }
    ::setrlimit(RLIMIT_FSIZE, &old);
    std::signal(SIGXFSZ, old_handler);
    REQUIRE(threw);

    CHECK(slurp(file) == h + "2026");
    CHECK_THROWS_AS(w.write_row({"t", "v"}), std::invalid_argument);  // closed

    CsvWriter again{file, test_schema()};  // lock was released by the close
    again.write_row({"2026-11-11T20:05:00Z", "2"});
    CHECK(slurp(file) == h + "2026\r\n2026-11-11T20:05:00Z,2\r\n");
}

TEST_CASE("CsvWriter checks the number of fields") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    CsvWriter w{file, test_schema()};
    CHECK_THROWS_AS(w.write_row({"only one"}), std::invalid_argument);
    CHECK_THROWS_AS(w.write_row({"a", "b", "c"}), std::invalid_argument);
    CHECK(slurp(file) == header_text(test_schema()));  // nothing written
}

TEST_CASE("CsvWriter rejects a schema without columns") {
    TempDir dir;
    CHECK_THROWS_AS(CsvWriter(dir.path / "x.csv", Schema{"x", 1, {}}), std::invalid_argument);
    CHECK_FALSE(fs::exists(dir.path / "x.csv"));
}

TEST_CASE("a second writer on the same file fails instead of interleaving rows") {
    TempDir dir;
    const auto file = dir.path / "test.csv";
    CsvWriter first{file, test_schema()};
    CHECK_THROWS_AS(CsvWriter(file, test_schema()), std::system_error);
    first.write_row({"t", "v"});  // the first writer is unaffected

    SUBCASE("the lock goes away with the writer") {
        { CsvWriter moved{std::move(first)}; }
        CsvWriter again{file, test_schema()};
        again.write_row({"t2", "v2"});
        CHECK(slurp(file) == header_text(test_schema()) + "t,v\r\nt2,v2\r\n");
    }
}

TEST_CASE("CsvWriter move leaves the source closed") {
    TempDir dir;
    CsvWriter a{dir.path / "a.csv", test_schema()};
    CsvWriter b{std::move(a)};
    CHECK_THROWS_AS(a.write_row({"t", "v"}), std::invalid_argument);  // NOLINT(bugprone-use-after-move)
    b.write_row({"t", "v"});

    CsvWriter c{dir.path / "c.csv", test_schema()};
    c = std::move(b);  // closes c.csv, takes over a.csv
    c.write_row({"t2", "v2"});
    CHECK(c.path() == dir.path / "a.csv");
    CHECK(slurp(dir.path / "a.csv") == header_text(test_schema()) + "t,v\r\nt2,v2\r\n");
    CsvWriter reopened{dir.path / "c.csv", test_schema()};  // lock on c.csv was released
}

TEST_CASE("CsvWriter reports a missing directory and a non-regular file") {
    TempDir dir;
    CHECK_THROWS_AS(CsvWriter(dir.path / "missing" / "x.csv", test_schema()), std::system_error);
    CHECK_THROWS_AS(CsvWriter(dir.path, test_schema()), std::system_error);
}
