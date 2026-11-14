/**
 * @file test_text.cpp
 * @brief Tests for the text helpers.
 */
#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include "core/text.hpp"
#include "support.hpp"

using meerkat::core::text::read_file;
using meerkat::test::TempDir;

TEST_CASE("read_file: contents, an empty file, and a missing one") {
    TempDir dir;
    std::ofstream{dir.path / "a.txt"} << "line 1\nline 2\n";
    CHECK(read_file(dir.path / "a.txt") == "line 1\nline 2\n");
    std::ofstream{dir.path / "empty.txt"};
    CHECK(read_file(dir.path / "empty.txt") == "");
    CHECK_FALSE(read_file(dir.path / "missing.txt"));

    const std::string big(10'000, 'x');  // more than one read() buffer
    std::ofstream{dir.path / "big.txt"} << big;
    CHECK(read_file(dir.path / "big.txt") == big);
}

TEST_CASE("read_file: a read error is no file, not an empty one (#51)") {
    TempDir dir;
    CHECK_FALSE(read_file(dir.path));  // a directory opens, but read() fails with EISDIR
    // Reading offset 0 of a process's memory fails (EIO): opens, then can't be read.
    CHECK_FALSE(read_file("/proc/self/mem"));
}
