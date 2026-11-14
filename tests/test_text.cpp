/**
 * @file test_text.cpp
 * @brief Tests for the text helpers.
 */
#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include "core/text.hpp"
#include "support.hpp"

using meerkat::core::text::is_host_argument;
using meerkat::core::text::leading_double;
using meerkat::core::text::parse_double;
using meerkat::core::text::read_file;
using meerkat::core::text::to_text;
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

TEST_CASE("leading_double and parse_double: nan and inf are no number (#44)") {
    // std::from_chars accepts all of these; tools print them when they have no figure.
    for (const char* s : {"nan", "NaN", "-nan", "nan(1)", "inf", "-inf", "INF", "infinity"}) {
        CAPTURE(s);
        CHECK_FALSE(parse_double(s));
        CHECK_FALSE(leading_double(s));
    }
    CHECK_FALSE(leading_double("nan dBm"));
    CHECK_FALSE(leading_double("inf MBit/s"));
    CHECK_FALSE(parse_double("1e999"));  // out of range
    CHECK(leading_double("-58 dBm") == doctest::Approx(-58.0));
    CHECK(parse_double("27.5") == doctest::Approx(27.5));
}

TEST_CASE("to_text: the exact number, in plain digits at everyday sizes (#44)") {
    CHECK(to_text(250) == "250");
    CHECK(to_text(27.5) == "27.5");
    CHECK(to_text(0.1) == "0.1");
    CHECK(to_text(-58) == "-58");
    CHECK(to_text(0) == "0");
    CHECK(to_text(1e6) == "1000000");          // was 1e+06
    CHECK(to_text(1234.5678) == "1234.5678");  // was 1234.57
    CHECK(to_text(0.000125) == "0.000125");
    CHECK(to_text(123456789012345) == "123456789012345");
    CHECK(to_text(-0.0) == "0");
    // Beyond 1e-6 … 1e15, the shorter form: no 301-digit message for a typo.
    CHECK(to_text(1e300) == "1e+300");
    CHECK(to_text(-1e-300) == "-1e-300");
    CHECK(to_text(1e15) == "1e+15");
    CHECK(to_text(1e-7) == "1e-07");
}

TEST_CASE("is_host_argument: host text that a program can't take for an option") {
    for (const char* ok : {"192.0.2.1", "2001:db8::1", "fe80::1%wlan0", "example.com", "a-b"}) {
        CAPTURE(ok);
        CHECK(is_host_argument(ok));
    }
    for (const char* bad : {"", "-f", "--help", "a b", "a\tb", "a\x7f"}) {
        CAPTURE(bad);
        CHECK_FALSE(is_host_argument(bad));
    }
}
