/**
 * @file test_official.cpp
 * @brief Tests for importing the Breitbandmessung app's files into the data
 *        directory, and sealing them (task 3.6).
 */
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <unistd.h>

#include "app/import.hpp"
#include "core/text.hpp"
#include "store/manifest.hpp"
#include "store/official.hpp"
#include "store/verify.hpp"
#include "sys/privileges.hpp"
#include "support.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using meerkat::core::Timestamp;
using meerkat::probe::LocalToUtc;
using meerkat::store::import_official;
using meerkat::test::TempDir;

namespace {

/// Local time is UTC+1.
const LocalToUtc kPlusOne = [](int y, int mo, int d, int h, int mi, int s) -> std::optional<Timestamp> {
    const std::chrono::year_month_day ymd{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(mo)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
    return Timestamp{std::chrono::sys_days{ymd} + std::chrono::hours{h} + std::chrono::minutes{mi} +
                     std::chrono::seconds{s} - 1h};
};

/// @return The rows of official.csv in @p data, without the two header lines.
std::vector<std::string> rows(const fs::path& data) {
    std::vector<std::string> out;
    const auto text = meerkat::core::text::read_file(data / "official.csv");
    REQUIRE(text);
    meerkat::core::text::for_each_line(*text, [&](std::string_view l) {
        if (l.ends_with('\r')) {
            l.remove_suffix(1);  // CsvWriter ends rows with CRLF
        }
        out.emplace_back(l);
    });
    out.erase(out.begin(), out.begin() + 2);
    return out;
}

/// Copies fixture @p name to @p dir / @p as.
fs::path copy_fixture(const fs::path& dir, const std::string& name, const std::string& as) {
    const auto to = dir / as;
    fs::copy_file(meerkat::test::fixture_path(name), to);
    return to;
}

}  // namespace

TEST_CASE("import_official: an export is kept as it is and its rows added once") {
    TempDir data;
    TempDir downloads;
    const auto src = copy_fixture(downloads.path, "bbm/three.csv", "Festnetz-Check_10_03_2026");

    auto r = import_official(data.path, src, kPlusOne);
    CHECK(r.stored == "official/Festnetz-Check_10_03_2026");
    CHECK_FALSE(r.already_stored);
    CHECK_FALSE(r.pdf);
    CHECK(r.measurements == 3);
    CHECK(r.added == 3);
    CHECK(meerkat::test::fixture("bbm/three.csv") ==
          *meerkat::core::text::read_file(data.path / "official" / "Festnetz-Check_10_03_2026"));
    CHECK((fs::status(data.path / r.stored).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::group_read));
    const auto lines = rows(data.path);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "2026-03-10T18:25:12Z,breitbandmessung,1,187.42,46.08,14.0,150.00,200.00,250.00,30.00,"
                      "40.00,50.00,LAN,yes,yes,no,"
                      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef,3.13.0 64-Bit,"
                      "official/Festnetz-Check_10_03_2026");

    SUBCASE("again: nothing new") {
        r = import_official(data.path, src, kPlusOne);
        CHECK(r.already_stored);
        CHECK(r.added == 0);
        CHECK(rows(data.path).size() == 3);
    }
    SUBCASE("another export with a measurement already in: only the new ones") {
        const auto single = copy_fixture(downloads.path, "bbm/single.csv", "single");
        r = import_official(data.path, single, kPlusOne);
        CHECK(r.measurements == 1);
        CHECK(r.added == 0);  // its Test-ID is the first row of three.csv
        CHECK(fs::exists(data.path / "official" / "single"));  // the file is still kept
    }
    SUBCASE("a different file under the same name is refused") {
        std::ofstream{downloads.path / "other"} << meerkat::test::fixture("bbm/single.csv");
        fs::remove(src);
        fs::rename(downloads.path / "other", src);
        CHECK_THROWS_WITH(import_official(data.path, src, kPlusOne), doctest::Contains("already holds a different file"));
    }
}

TEST_CASE("import_official: the PDF protocol is kept, not read; other files are refused") {
    TempDir data;
    TempDir downloads;
    auto r = import_official(data.path, copy_fixture(downloads.path, "bbm/protocol.pdf", "messung.pdf"), kPlusOne);
    CHECK(r.pdf);
    CHECK(r.stored == "official/messung.pdf");
    CHECK_FALSE(fs::exists(data.path / "official.csv"));

    std::ofstream{downloads.path / "other.pdf"} << "%PDF-1.4\n% some other document\n";
    CHECK_THROWS_WITH(import_official(data.path, downloads.path / "other.pdf", kPlusOne),
                      doctest::Contains("not one the Breitbandmessung app"));
    std::ofstream{downloads.path / "notes.txt"} << "hello\n";
    CHECK_THROWS_AS(import_official(data.path, downloads.path / "notes.txt", kPlusOne),
                    meerkat::probe::BbmFormatError);
    CHECK_FALSE(fs::exists(data.path / "official" / "notes.txt"));  // refused before anything was written
    CHECK_THROWS(import_official(data.path, downloads.path / "missing.csv", kPlusOne));
}

TEST_CASE("the manifest seals the official files; a changed one is found") {
    TempDir data;
    TempDir downloads;
    import_official(data.path, copy_fixture(downloads.path, "bbm/single.csv", "export.csv"), kPlusOne);
    import_official(data.path, copy_fixture(downloads.path, "bbm/protocol.pdf", "messung.pdf"), kPlusOne);
    std::ofstream{data.path / "official" / ".messung.pdf.tmp-1"} << "half";  // still being written: not sealed
    CHECK(meerkat::store::official_files(data.path) ==
          std::vector<std::string>{"official/export.csv", "official/messung.pdf"});

    auto files = meerkat::store::daily_files("2026-03-10");
    for (auto& f : meerkat::store::official_files(data.path)) {
        files.push_back(std::move(f));
    }
    const auto m = meerkat::store::build_manifest(data.path, "2026-03-10", Timestamp{1'773'187'205s}, files);
    std::vector<std::string> sealed;
    for (const auto& e : m.entries) {
        sealed.push_back(e.path);
    }
    CHECK(sealed == std::vector<std::string>{"official.csv", "official/export.csv", "official/messung.pdf"});
    const auto before = meerkat::store::verify_data(data.path, std::chrono::sys_days{std::chrono::year{2026} / 3 / 11});
    bool noted = false;
    for (const auto& f : before.findings) {
        noted = noted || f.text == "official/messung.pdf: imported after the last manifest, not sealed yet";
    }
    CHECK(noted);
    meerkat::store::write_manifest(data.path, m);
    const auto today = std::chrono::sys_days{std::chrono::year{2026} / 3 / 11};
    CHECK(meerkat::store::verify_data(data.path, today).ok());

    // An exported file must stay as it was, even at its end.
    fs::permissions(data.path / "official" / "messung.pdf", fs::perms::owner_write, fs::perm_options::add);
    std::ofstream{data.path / "official" / "messung.pdf", std::ios::app} << "% added\n";
    CHECK_FALSE(meerkat::store::verify_data(data.path, today).ok());
}

TEST_CASE("import_bbm_command: reports each file, and fails if one fails") {
    TempDir data;
    TempDir downloads;
    const auto good = copy_fixture(downloads.path, "bbm/single.csv", "export.csv");
    std::ostringstream out;
    std::ostringstream err;
    CHECK(meerkat::app::import_bbm_command(data.path, {good, downloads.path / "missing"}, kPlusOne, out, err) == 1);
    CHECK(out.str().find("export.csv: 1 measurement, 1 new in official.csv; the file is kept as official/export.csv") !=
          std::string::npos);
    CHECK(err.str().find("missing: ") != std::string::npos);

    std::ostringstream out2;
    CHECK(meerkat::app::import_bbm_command(data.path / "nope", {good}, kPlusOne, out2, err) == 1);
    CHECK(err.str().find("no data directory") != std::string::npos);
}

TEST_CASE("import_official: names a manifest can't hold are refused (#129)") {
    TempDir data;
    TempDir downloads;
    for (const auto* name : {"a\nb.csv", "a\rb.csv", ".hidden.csv", "tab\there.csv"}) {
        CAPTURE(name);
        const auto src = copy_fixture(downloads.path, "bbm/single.csv", name);
        CHECK_THROWS_WITH(import_official(data.path, src, kPlusOne), doctest::Contains("rename it"));
    }
    CHECK_FALSE(fs::exists(data.path / "official"));
    CHECK(meerkat::store::importable_name("Festnetz-Check_16_11_2026 (1).csv"));
    CHECK(meerkat::store::importable_name("Messung äöü.pdf"));

    // Put there by hand: never handed to a manifest.
    fs::create_directories(data.path / "official");
    std::ofstream{data.path / "official" / "x\ny"} << "z";
    CHECK(meerkat::store::official_files(data.path).empty());
}

TEST_CASE("official_files: a folder that can't be listed is an error, not empty (#129)") {
    if (::geteuid() == 0) {
        return;  // root lists it anyway
    }
    TempDir data;
    CHECK(meerkat::store::official_files(data.path).empty());  // none yet: fine
    fs::create_directories(data.path / "official");
    fs::permissions(data.path / "official", fs::perms::none);
    CHECK_THROWS_AS(meerkat::store::official_files(data.path), std::system_error);
    bool reported = false;
    for (const auto& f : meerkat::store::verify_data(data.path, std::chrono::sys_days{std::chrono::year{2026} / 3 / 11})
                             .findings) {
        reported = reported || (f.level == meerkat::store::VerifyFinding::Level::problem &&
                                f.text.find("can't list") != std::string::npos);
    }
    CHECK(reported);
    fs::permissions(data.path / "official", fs::perms::owner_all);
}

TEST_CASE("import_official: never through a symlinked official/ (#133)") {
    TempDir data;
    TempDir elsewhere;
    TempDir downloads;
    fs::create_directory_symlink(elsewhere.path, data.path / "official");
    const auto src = copy_fixture(downloads.path, "bbm/single.csv", "export.csv");
    CHECK_THROWS_WITH(import_official(data.path, src, kPlusOne), doctest::Contains("isn't a directory (a symlink?)"));
    CHECK(fs::is_empty(elsewhere.path));
    CHECK((fs::status(elsewhere.path).permissions() & fs::perms::all) == (fs::perms::owner_all));  // untouched (TempDir: 0700)
}

TEST_CASE("owner_to_become: nothing to drop unless root") {
    TempDir data;
    if (::geteuid() != 0) {
        CHECK_FALSE(meerkat::sys::owner_to_become(data.path));
    }
}
