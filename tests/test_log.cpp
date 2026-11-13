/**
 * @file test_log.cpp
 * @brief Tests for app::Log and detect_log_style().
 */
#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sstream>
#include <string>

#include "app/log.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace meerkat::app;

namespace {

/// A fixed clock: 2026-01-02T03:04:05Z.
core::Timestamp fixed() { return *core::parse_iso8601("2026-01-02T03:04:05Z"); }

}  // namespace

TEST_CASE("Log: plain lines with time and level") {
    std::ostringstream out;
    Log log{out, LogStyle::plain, fixed};
    log.warning("the connection is Wi-Fi");
    log.info("speed test");
    log.status("ignored outside a terminal");
    log.error("disk full");
    log.notice("stopped");
    CHECK(out.str() ==
          "2026-01-02T03:04:05Z warning: the connection is Wi-Fi\n"
          "2026-01-02T03:04:05Z info: speed test\n"
          "2026-01-02T03:04:05Z error: disk full\n"
          "2026-01-02T03:04:05Z notice: stopped\n");
}

TEST_CASE("Log: journald priorities, no timestamps") {
    std::ostringstream out;
    Log log{out, LogStyle::journal, fixed};
    log.error("a");
    log.warning("b");
    log.notice("c");
    log.info("d");
    log.status("ignored");
    CHECK(out.str() == "<3>a\n<4>b\n<5>c\n<6>d\n");
}

TEST_CASE("Log: messages escape control characters in every style") {
    std::string message = "first\nsecond\rthird\ttab";
    message.push_back('\b');
    message.push_back('\0');
    message += "tail";
    message.push_back('\x1B');
    message.push_back('\x7F');
    const std::string safe = "first\\nsecond\\rthird\\ttab\\x08\\x00tail\\x1B\\x7F";

    std::ostringstream plain_out;
    Log plain{plain_out, LogStyle::plain, fixed};
    plain.error(message);
    CHECK(plain_out.str() == "2026-01-02T03:04:05Z error: " + safe + "\n");

    std::ostringstream journal_out;
    Log journal{journal_out, LogStyle::journal, fixed};
    journal.error(message);
    CHECK(journal_out.str() == "<3>" + safe + "\n");

    std::ostringstream terminal_out;
    Log terminal{terminal_out, LogStyle::terminal, fixed};
    terminal.status("progress");
    terminal.error(message);
    CHECK(terminal_out.str() == "progress\r\033[K2026-01-02T03:04:05Z error: " + safe +
                                "\nprogress");
}

TEST_CASE("Log: status text escapes line breaks and terminal controls") {
    std::ostringstream out;
    Log log{out, LogStyle::terminal, fixed};
    log.status("step\nnext\r\x1B");
    CHECK(out.str() == "step\\nnext\\r\\x1B");
}

TEST_CASE("Log: the terminal status line stays below the log") {
    std::ostringstream out;
    Log log{out, LogStyle::terminal, fixed};
    log.info("one");
    log.status("status 1");
    log.info("two");
    log.status("status 2");
    log.clear_status();
    CHECK(out.str() ==
          "2026-01-02T03:04:05Z info: one\n"
          "status 1"
          "\r\033[K2026-01-02T03:04:05Z info: two\nstatus 1"
          "\r\033[Kstatus 2"
          "\r\033[K");
}

TEST_CASE("detect_log_style") {
    meerkat::test::TempDir dir;
    const auto file = dir.path / "log";
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    struct stat st {};
    REQUIRE(::fstat(fd, &st) == 0);
    const auto stream = std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino);

    CHECK(detect_log_style(fd, stream) == LogStyle::journal);
    CHECK(detect_log_style(fd, "") == LogStyle::plain);  // a file, not a TTY
    CHECK(detect_log_style(fd, std::to_string(st.st_dev) + ":1") == LogStyle::plain);
    CHECK(detect_log_style(fd, "garbage") == LogStyle::plain);
    CHECK(detect_log_style(fd, "1:x") == LogStyle::plain);
    ::close(fd);
}

TEST_CASE("Log: the status line is cut to the terminal width") {
    std::ostringstream out;
    std::size_t columns = 10;
    Log log{out, LogStyle::terminal, fixed, [&columns] { return columns; }};
    log.status("speed 12:00 – net up");
    CHECK(out.str() == "speed 12:");  // width - 1 = 9 characters
    columns = 0;                        // unknown width: no limit
    log.status("abc");
    CHECK(out.str() == "speed 12:\r\033[Kabc");
}

TEST_CASE("truncate_utf8 counts characters, not bytes") {
    CHECK(truncate_utf8("abc", 5) == "abc");
    CHECK(truncate_utf8("abc", 2) == "ab");
    CHECK(truncate_utf8("a–b", 2) == "a–");  // – is 3 bytes
    CHECK(truncate_utf8("a–b", 1) == "a");
    CHECK(truncate_utf8("", 0).empty());
    CHECK(terminal_columns(-1) == 0);
}
