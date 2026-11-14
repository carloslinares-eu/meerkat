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

TEST_CASE("visible: control characters and invalid UTF-8 become \\xNN (#39, #60)") {
    using meerkat::core::text::visible;
    CHECK(visible("") == "");
    CHECK(visible("plain text 1.1.1.1") == "plain text 1.1.1.1");
    CHECK(visible("line one\nline two") == "line one\\x0aline two");
    CHECK(visible("a\r\tb") == "a\\x0d\\x09b");
    CHECK(visible("\x1b[2Jcleared") == "\\x1b[2Jcleared");
    CHECK(visible(std::string{"nul\0byte", 8}) == "nul\\x00byte");
    CHECK(visible("example.com\x7f") == "example.com\\x7f");
    CHECK(visible("C:\\path") == "C:\\\\path");  // unambiguous: \x0a in the input stays apart
    CHECK(visible("\\x0a") == "\\\\x0a");
    // Valid UTF-8 stays.
    CHECK(visible("Grüße – 250 Mbit/s 🚀") == "Grüße – 250 Mbit/s 🚀");
    CHECK(visible("\u00a0") == "\u00a0");  // NBSP, the first character after C1
    // C1 controls (U+0080–U+009F; U+009B is CSI on some terminals).
    CHECK(visible("a\xc2\x9b" "2J") == "a\\xc2\\x9b2J");
    CHECK(visible("\xc2\x85") == "\\xc2\\x85");
    // Not UTF-8: one escape per bad byte, the rest kept.
    CHECK(visible("\xff") == "\\xff");
    CHECK(visible("\x80x") == "\\x80x");                       // stray continuation
    CHECK(visible("\xe2\x80") == "\\xe2\\x80");                // cut off
    CHECK(visible("\xc0\xaf") == "\\xc0\\xaf");                // overlong '/'
    CHECK(visible("\xed\xa0\x80") == "\\xed\\xa0\\x80");       // surrogate
    CHECK(visible("\xf4\x90\x80\x80") == "\\xf4\\x90\\x80\\x80");  // beyond U+10FFFF
    CHECK(visible("\xe2\x80" "a–") == "\\xe2\\x80a–");
    // Invisible characters that change how text reads.
    CHECK(visible("Example\u202eteN") == "Example\\xe2\\x80\\xaeteN");  // RLO
    CHECK(visible("a\u200bb") == "a\\xe2\\x80\\x8bb");                // zero-width space
    CHECK(visible("a\u2028b") == "a\\xe2\\x80\\xa8b");                // line separator
    CHECK(visible("\ufeffx") == "\\xef\\xbb\\xbfx");                  // BOM
    CHECK(visible("\u2069") == "\\xe2\\x81\\xa9");                     // PDI
    CHECK(visible("a\u2010b\u2030") == "a\u2010b\u2030");  // hyphen, per mille: shown
}

TEST_CASE("utf8_length") {
    using meerkat::core::text::utf8_length;
    CHECK(utf8_length("a") == 1);
    CHECK(utf8_length("ä") == 2);
    CHECK(utf8_length("–") == 3);
    CHECK(utf8_length("🚀") == 4);
    CHECK(utf8_length("\xf4\x8f\xbf\xbf") == 4);  // U+10FFFF
    CHECK(utf8_length("\xe0\x9f\x80") == 0);      // overlong
    CHECK(utf8_length("\xf0\x8f\x80\x80") == 0);  // overlong
    CHECK(utf8_length("\xc1\x81") == 0);
}
