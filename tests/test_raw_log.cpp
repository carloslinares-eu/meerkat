/**
 * @file test_raw_log.cpp
 * @brief Tests for store::RawLog and the raw-log record format.
 */
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <string>

#include "store/raw_log.hpp"
#include "support.hpp"

using namespace meerkat;
using namespace meerkat::store;
using meerkat::test::TempDir;
using nlohmann::json;

namespace {

/// A UTC timestamp.
core::Timestamp at(int day, int hour, int minute = 0, int second = 0) {
    using namespace std::chrono;
    return sys_days{year{2026} / 11 / day} + hours{hour} + minutes{minute} + seconds{second};
}

/// A speed measurement whose backend printed @p raw.
core::Measurement<core::SpeedResult> speed(core::Timestamp t, std::string raw,
                                           core::Status status = core::Status::ok) {
    core::Measurement<core::SpeedResult> m;
    m.time = t;
    m.result.status = status;
    m.result.backend = "ookla";
    m.result.raw = std::move(raw);
    return m;
}

/// The whole file.
std::string slurp(const std::filesystem::path& p) { return core::text::read_file(p).value(); }

}  // namespace

TEST_CASE("utc_day") {
    CHECK(utc_day(at(11, 0)) == "2026-11-11");
    CHECK(utc_day(at(11, 23, 59, 59)) == "2026-11-11");
    CHECK(utc_day(at(12, 0)) == "2026-11-12");
}

TEST_CASE("base64 (RFC 4648 test vectors)") {
    CHECK(base64("") == "");
    CHECK(base64("f") == "Zg==");
    CHECK(base64("fo") == "Zm8=");
    CHECK(base64("foo") == "Zm9v");
    CHECK(base64("foob") == "Zm9vYg==");
    CHECK(base64("fooba") == "Zm9vYmE=");
    CHECK(base64("foobar") == "Zm9vYmFy");
    CHECK(base64(std::string{"\xff\xfe\x00", 3}) == "//4A");
}

TEST_CASE("raw_record keeps the output byte for byte") {
    const std::string out =
        "{\"type\":\"result\",\"isp\":\"Example ISP\",\"note\":\"tab\\there\"}";
    const auto line = raw_record(speed(at(11, 20, 5), out));
    CHECK(line.find('\n') == std::string::npos);
    const auto j = json::parse(line);
    CHECK(j.at("time") == "2026-11-11T20:05:00Z");
    CHECK(j.at("backend") == "ookla");
    CHECK(j.at("status") == "ok");
    CHECK(j.at("error") == "");
    CHECK(j.at("output").get<std::string>() == out);
    CHECK_FALSE(j.contains("output_base64"));
    // Field order is fixed, so lines are easy to read and diff.
    CHECK(line.rfind("{\"time\":", 0) == 0);

    SUBCASE("multi-line output stays on one line") {
        const std::string err = "[error] Configuration - Couldn't resolve host name\n[error] x\n";
        const auto l = raw_record(speed(at(11, 20), err, core::Status::no_network));
        CHECK(l.find('\n') == std::string::npos);
        CHECK(json::parse(l).at("output").get<std::string>() == err);
        CHECK(json::parse(l).at("status") == "no_network");
    }
    SUBCASE("invalid UTF-8 goes into output_base64") {
        const std::string bad{"ok \xc3\x28 bad", 9};
        const auto j2 = json::parse(raw_record(speed(at(11, 20), bad)));
        CHECK_FALSE(j2.contains("output"));
        CHECK(j2.at("output_base64") == base64(bad));
    }
    SUBCASE("UTF-8 text is kept as text") {
        const std::string text = "Server: Köln – Beispiel";
        CHECK(json::parse(raw_record(speed(at(11, 20), text))).at("output") == text);
    }
}

TEST_CASE("RawLog writes one file per UTC day") {
    TempDir dir;
    const auto raw = dir.path / "raw";
    RawLog log{raw};
    CHECK(std::filesystem::is_directory(raw));

    log.append(speed(at(11, 23, 55), "a"));
    log.append(speed(at(11, 23, 59, 59), "b"));
    log.append(speed(at(12, 0, 0, 1), "c"));

    const auto day1 = slurp(log.file_for("2026-11-11"));
    const auto day2 = slurp(log.file_for("2026-11-12"));
    CHECK(day1 == raw_record(speed(at(11, 23, 55), "a")) + "\n" +
                      raw_record(speed(at(11, 23, 59, 59), "b")) + "\n");
    CHECK(day2 == raw_record(speed(at(12, 0, 0, 1), "c")) + "\n");
    CHECK(log.file_for("2026-11-11") == raw / "2026-11-11.jsonl");

    SUBCASE("a second log on the same day's file is refused while the first is open") {
        RawLog other{raw};
        CHECK_THROWS_AS(other.append(speed(at(12, 1), "d")), std::system_error);
    }
}

TEST_CASE("RawLog appends to an existing file and ends a torn line") {
    TempDir dir;
    const auto file = dir.path / "2026-11-11.jsonl";
    {
        std::ofstream out{file, std::ios::binary};
        out << "{\"time\":\"2026-11-11T20:00:00Z\",\"bac";  // the last run died here
    }
    RawLog log{dir.path};
    log.append(speed(at(11, 20, 5), "x"));
    const auto text = slurp(file);
    CHECK(text == "{\"time\":\"2026-11-11T20:00:00Z\",\"bac\n" +
                      raw_record(speed(at(11, 20, 5), "x")) + "\n");

    SUBCASE("a complete file is appended to unchanged") {
        RawLog again{dir.path};
        log = RawLog{dir.path};  // release the lock held by `log`
        again.append(speed(at(11, 20, 10), "y"));
        CHECK(slurp(file) == text + raw_record(speed(at(11, 20, 10), "y")) + "\n");
    }
}
