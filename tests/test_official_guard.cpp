/**
 * @file test_official_guard.cpp
 * @brief Tests for core::OfficialGuard, the procfs process list and the
 *        pause file (task 2.10).
 */
#include <doctest/doctest.h>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <string>
#include <vector>

#include "core/official_guard.hpp"
#include "core/text.hpp"
#include "store/pause_file.hpp"
#include "sys/proc_list.hpp"
#include "support.hpp"

using namespace meerkat;
using meerkat::test::TempDir;
namespace fs = std::filesystem;

namespace {

/// Parses an ISO 8601 time; fails the test if invalid.
core::Timestamp at(const char* iso) {
    const auto t = core::parse_iso8601(iso);
    REQUIRE(t);
    return *t;
}

/**
 * @brief Adds a process to a fake /proc tree.
 * @param proc    The tree.
 * @param pid     Its directory name.
 * @param cmdline The cmdline file, NUL-separated.
 * @param comm    The comm file, with its newline.
 */
void fake_process(const fs::path& proc, const std::string& pid, const std::string& cmdline,
                  const std::string& comm) {
    fs::create_directories(proc / pid);
    std::ofstream{proc / pid / "cmdline", std::ios::binary} << cmdline;
    std::ofstream{proc / pid / "comm", std::ios::binary} << comm;
}

}  // namespace

TEST_CASE("OfficialGuard: pauses while the app runs, and says so on each change") {
    core::OfficialGuard g;
    const auto t = at("2026-03-10T12:00:00Z");

    auto c = g.check({}, std::nullopt, t);
    CHECK_FALSE(c.pause);
    CHECK_FALSE(c.notice);  // nothing to say at start when all is normal

    c = g.check({"breitbandmessung"}, std::nullopt, t);
    REQUIRE(c.pause);
    CHECK(*c.pause == "the Breitbandmessung app is running (breitbandmessung)");
    CHECK(c.notice == "speed tests paused: the Breitbandmessung app is running (breitbandmessung)");

    c = g.check({"breitbandmessung"}, std::nullopt, t);
    CHECK(c.pause);
    CHECK_FALSE(c.notice);  // not repeated

    c = g.check({}, std::nullopt, t);
    CHECK_FALSE(c.pause);
    CHECK(c.notice == "the Breitbandmessung app has stopped; speed tests resume");

    c = g.check({"a", "b"}, std::nullopt, t);
    CHECK(c.pause == "the Breitbandmessung app is running (a, b)");
}

TEST_CASE("OfficialGuard: a manual pause lasts until its time") {
    core::OfficialGuard g;
    const auto until = at("2026-03-10T14:00:00Z");

    auto c = g.check({}, until, at("2026-03-10T12:00:00Z"));
    CHECK(c.pause == "paused with `meerkat pause` until 2026-03-10T14:00:00Z");
    CHECK(c.notice);

    SUBCASE("it ends at its time") {
        c = g.check({}, until, until);
        CHECK_FALSE(c.pause);
        CHECK(c.notice == "the pause is over; speed tests resume");
    }
    SUBCASE("the app starts meanwhile: the app is named, then the pause again") {
        c = g.check({"breitbandmessung"}, until, at("2026-03-10T12:05:00Z"));
        CHECK(c.pause == "the Breitbandmessung app is running (breitbandmessung)");
        CHECK(c.notice);
        c = g.check({}, until, at("2026-03-10T12:10:00Z"));
        CHECK(c.pause == "paused with `meerkat pause` until 2026-03-10T14:00:00Z");
    }
    SUBCASE("a pause in the past is none") {
        core::OfficialGuard fresh;
        c = fresh.check({}, at("2026-03-10T11:00:00Z"), at("2026-03-10T12:00:00Z"));
        CHECK_FALSE(c.pause);
        CHECK_FALSE(c.notice);
    }
}

TEST_CASE("list_processes: reads argv[0] and comm from procfs") {
    TempDir dir;
    const auto proc = dir.path / "proc";
    fake_process(proc, "1", std::string{"/sbin/init\0splash\0", 18}, "systemd\n");
    fake_process(proc, "4242", std::string{"/opt/Festnetz-Check/breitbandmessung\0--no-sandbox\0", 50},
                 "breitbandmessun\n");
    fake_process(proc, "4243", "/opt/Festnetz-Check/breitbandmessung --type=renderer",
                 "breitbandmessun\n");                 // Chromium rewrote its cmdline
    fake_process(proc, "2", "", "kthreadd\n");         // kernel thread
    fake_process(proc, "self", "x", "x\n");            // not a pid
    fs::create_directories(proc / "999");              // gone while listing: no files

    auto list = sys::list_processes(proc);
    std::sort(list.begin(), list.end(),
              [](const auto& a, const auto& b) { return a.pid < b.pid; });
    REQUIRE(list.size() == 4);
    CHECK(list[0].pid == 1);
    CHECK(list[0].name == "init");
    CHECK(list[0].comm == "systemd");
    CHECK(list[1].name.empty());
    CHECK(list[2].name == "breitbandmessung");
    CHECK(list[3].name == "breitbandmessung --type=renderer");

    CHECK(sys::running_programs(list, {"breitbandmessung", "init", "nothere"}) ==
          std::vector<std::string>{"breitbandmessung", "init"});
    CHECK(sys::running_programs(list, {}).empty());
    CHECK(sys::list_processes(dir.path / "missing").empty());
}

TEST_CASE("process_is: argv[0] file name, or comm cut to 15 characters") {
    const sys::ProcessEntry helper{7, "breitbandmessung --type=gpu-process", "breitbandmessun"};
    CHECK(sys::process_is(helper, "breitbandmessung"));
    CHECK_FALSE(sys::process_is(helper, "breitband"));  // no prefix matches
    // Another program sharing the first 15 characters: its cmdline decides.
    CHECK_FALSE(sys::process_is({8, "breitbandmessung-updater", "breitbandmessun"},
                                "breitbandmessung"));
    CHECK(sys::process_is({9, "", "short"}, "short"));
    CHECK_FALSE(sys::process_is({9, "", ""}, ""));
}

TEST_CASE("live: list_processes finds this test process") {
    const auto list = sys::list_processes();
    const auto self = std::find_if(list.begin(), list.end(),
                                   [](const auto& p) { return p.pid == ::getpid(); });
    REQUIRE(self != list.end());
    CHECK(self->name == "meerkat_tests");
}

TEST_CASE("pause file: written, read back, replaced, removed") {
    TempDir dir;
    CHECK_FALSE(store::read_pause(dir.path));
    store::write_pause(dir.path, at("2026-03-10T14:00:00Z"));
    CHECK(core::text::read_file(dir.path / "pause") == "2026-03-10T14:00:00Z\n");
    CHECK(store::read_pause(dir.path) == at("2026-03-10T14:00:00Z"));
    store::write_pause(dir.path, at("2026-03-10T16:00:00Z"));
    CHECK(store::read_pause(dir.path) == at("2026-03-10T16:00:00Z"));
    CHECK(fs::status(dir.path / "pause").permissions() ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
           fs::perms::others_read));  // the service may be another user
    CHECK(store::remove_pause(dir.path));
    CHECK_FALSE(store::remove_pause(dir.path));
    CHECK_FALSE(store::read_pause(dir.path));
    // Only the pause file is left behind, no temp files.
    CHECK(fs::is_empty(dir.path));
}

TEST_CASE("pause file: garbage and symlinks are errors, not a silent no-pause") {
    TempDir dir;
    std::ofstream{dir.path / "pause"} << "tomorrow\n";
    CHECK_THROWS_WITH_AS(store::read_pause(dir.path),
                         doctest::Contains("doesn't hold a time"), std::runtime_error);
    std::ofstream{dir.path / "pause", std::ios::trunc} << "";
    CHECK_THROWS_AS(store::read_pause(dir.path), std::runtime_error);
    fs::remove(dir.path / "pause");

    std::ofstream{dir.path / "elsewhere"} << "2026-03-10T14:00:00Z\n";
    fs::create_symlink(dir.path / "elsewhere", dir.path / "pause");
    CHECK_THROWS_AS(store::read_pause(dir.path), std::system_error);
    // Writing replaces the link, not the file it points to.
    store::write_pause(dir.path, at("2026-03-10T16:00:00Z"));
    CHECK(fs::is_regular_file(fs::symlink_status(dir.path / "pause")));
    CHECK(core::text::read_file(dir.path / "elsewhere") == "2026-03-10T14:00:00Z\n");
}
