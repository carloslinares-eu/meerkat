/**
 * @file test_zip.cpp
 * @brief Tests for the ZIP writer and sys::AtomicFile.
 */
#include <doctest/doctest.h>

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "store/zip.hpp"
#include "support.hpp"
#include "sys/atomic_file.hpp"
#include "zip_reader.hpp"

using namespace meerkat;
namespace fs = std::filesystem;

namespace {

/// 2026-03-10T14:30:08Z.
core::Timestamp when() {
    using namespace std::chrono;
    return sys_days{year{2026} / 3 / 10} + hours{14} + minutes{30} + seconds{8};
}

/// A text that doesn't compress to nothing and spans several chunks.
std::string big_text() {
    std::string s;
    for (int i = 0; i < 20000; ++i) {
        s += "2026-03-10T00:00:00Z,ok," + std::to_string(i * 7919 % 100003) + "\r\n";
    }
    return s;
}

}  // namespace

TEST_CASE("ZipWriter: entries read back unchanged") {
    std::string zip;
    store::ZipWriter w{[&zip](std::string_view b) { zip += b; }, when()};
    const auto big = big_text();
    w.add("bundle/README.txt", "hello\n");
    w.add("bundle/empty", "");
    w.add("bundle/data/speed.csv", big);
    w.finish();
    CHECK(zip.size() < big.size() / 2);  // deflated

    const auto entries = test::read_zip(zip);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].name == "bundle/README.txt");
    CHECK(entries[0].data == "hello\n");
    CHECK(entries[1].data.empty());
    CHECK(entries[2].data == big);
    CHECK(entries[2].method == 8);
    CHECK(entries[2].flags == ((1u << 3) | (1u << 11)));
    CHECK((entries[2].external >> 16) == 0100444u);

    // The DOS time: 14:30:08 on 2026-03-10.
    const auto t = test::le16(zip, 10);
    const auto d = test::le16(zip, 12);
    CHECK((t >> 11) == 14);
    CHECK(((t >> 5) & 63) == 30);
    CHECK((t & 31) == 4);
    CHECK((d >> 9) == 2026 - 1980);
    CHECK(((d >> 5) & 15) == 3);
    CHECK((d & 31) == 10);
}

TEST_CASE("ZipWriter: an empty archive and finish twice") {
    std::string zip;
    store::ZipWriter w{[&zip](std::string_view b) { zip += b; }, when()};
    w.finish();
    w.finish();
    CHECK(zip.size() == 22);
    CHECK(test::read_zip(zip).empty());
    CHECK_THROWS_AS(w.add("x", "y"), std::logic_error);
}

TEST_CASE("ZipWriter: names must be plain relative paths") {
    std::string zip;
    store::ZipWriter w{[&zip](std::string_view b) { zip += b; }, when()};
    for (const auto* bad : {"", "/etc/passwd", "a/../b", "./a", "a//b", "a/", "a\\b", "a\nb", ".."}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(w.add(bad, "x"), std::invalid_argument);
    }
    CHECK_NOTHROW(w.add("a/b.c", "x"));
    CHECK_NOTHROW(w.add("ünï/cödé", "x"));
}

TEST_CASE("ZipWriter: add_file takes the first size bytes") {
    test::TempDir dir;
    const auto file = dir.path / "f.csv";
    const auto big = big_text();
    {
        std::ofstream out{file, std::ios::binary};
        out << big << "half a ro";
    }
    std::string zip;
    store::ZipWriter w{[&zip](std::string_view b) { zip += b; }, when()};
    w.add_file("f.csv", file, big.size());
    CHECK_THROWS_AS(w.add_file("g.csv", file, big.size() + 100), std::runtime_error);
    CHECK_THROWS(w.add_file("h.csv", dir.path / "nope", 1));
    fs::create_symlink(file, dir.path / "link");
    CHECK_THROWS(w.add_file("i.csv", dir.path / "link", 1));
}

TEST_CASE("AtomicFile: appears on commit only, mode 0600") {
    test::TempDir dir;
    const auto target = dir.path / "out.zip";
    {
        sys::AtomicFile f{target};
        f.write("abc");
        CHECK_FALSE(fs::exists(target));
    }  // no commit: gone
    CHECK_FALSE(fs::exists(target));
    CHECK(fs::is_empty(dir.path));

    {
        std::ofstream old{target};
        old << "old";
    }
    sys::AtomicFile f{target};
    f.write("new");
    f.write(" content");
    f.commit();
    CHECK(*core::text::read_file(target) == "new content");
    struct stat st{};
    REQUIRE(::stat(target.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(std::distance(fs::directory_iterator{dir.path}, fs::directory_iterator{}) == 1);

    CHECK_THROWS_AS(sys::AtomicFile{dir.path / "no" / "such" / "dir"}, std::system_error);
}
