/**
 * @file test_process.cpp
 * @brief Tests for sys::run() and sys::find_executable().
 */
#include <doctest/doctest.h>

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "core/text.hpp"
#include "sys/process.hpp"

using namespace std::chrono_literals;
using meerkat::sys::Command;
using meerkat::sys::find_executable;
using meerkat::sys::ProcessResult;
using meerkat::sys::run;
using Outcome = ProcessResult::Outcome;

TEST_CASE("captures stdout and a zero exit code") {
    const auto r = run({.program = "echo", .args = {"hello", "world"}});
    CHECK(r.ok());
    CHECK(r.outcome == Outcome::exited);
    CHECK(r.out == "hello world\n");
    CHECK(r.err.empty());
}

TEST_CASE("reports a non-zero exit code and stderr separately") {
    const auto r = run({.program = "sh", .args = {"-c", "echo out; echo err >&2; exit 3"}});
    CHECK_FALSE(r.ok());
    CHECK(r.outcome == Outcome::exited);
    CHECK(r.exit_code == 3);
    CHECK(r.out == "out\n");
    CHECK(r.err == "err\n");
}

TEST_CASE("arguments are passed literally, never through a shell") {
    const auto r = run({.program = "printf", .args = {"%s|", "a b", "$HOME", ";", "`id`", "*"}});
    CHECK(r.ok());
    CHECK(r.out == "a b|$HOME|;|`id`|*|");
}

TEST_CASE("a missing program is reported, not thrown") {
    const auto r = run({.program = "meerkat-no-such-program-xyz"});
    CHECK(r.outcome == Outcome::spawn_failed);
    CHECK(r.not_found);
    CHECK(r.error.find("not found") != std::string::npos);
}

TEST_CASE("a program that can't be executed is reported with errno text") {
    const auto dir = std::filesystem::temp_directory_path() / ("meerkat-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto script = dir / "noexec-format";
    std::ofstream{script} << "\x7f" "ELF garbage";
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    const auto r = run({.program = script.string()});
    CHECK(r.outcome == Outcome::spawn_failed);
    CHECK_FALSE(r.not_found);
    CHECK(r.error.find("cannot run") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_CASE("stdin is /dev/null, so readers don't hang") {
    const auto r = run({.program = "cat", .timeout = 5s});
    CHECK(r.ok());
    CHECK(r.out.empty());
}

TEST_CASE("environment overrides are applied, the rest is inherited") {
    ::setenv("MEERKAT_TEST_INHERITED", "yes", 1);
    const auto r = run({.program = "sh",
                        .args = {"-c", "echo \"$LC_ALL/$MEERKAT_TEST_INHERITED\""},
                        .env = {{"LC_ALL", "C"}}});
    CHECK(r.out == "C/yes\n");
}

TEST_CASE("a timeout stops the program with SIGTERM") {
    const auto r = run({.program = "sleep", .args = {"10"}, .timeout = 200ms});
    CHECK(r.outcome == Outcome::timed_out);
    CHECK(r.signal == SIGTERM);
    CHECK(r.duration < 3s);
}

TEST_CASE("a program that ignores SIGTERM gets SIGKILL after the grace period") {
    const auto r = run({.program = "sh",
                        .args = {"-c", "trap '' TERM; while :; do sleep 0.05; done"},
                        .timeout = 200ms,
                        .kill_grace = 200ms});
    CHECK(r.outcome == Outcome::timed_out);
    CHECK(r.signal == SIGKILL);
    CHECK(r.duration < 3s);
}

TEST_CASE("a background child holding the pipes doesn't block the run") {
    const auto r = run({.program = "sh", .args = {"-c", "sleep 10 & echo done"}, .timeout = 20s});
    CHECK(r.ok());
    CHECK(r.out == "done\n");
    CHECK(r.duration < 3s);
}

TEST_CASE("a program killed by a signal is reported as signaled") {
    const auto r = run({.program = "sh", .args = {"-c", "kill -KILL $$"}});
    CHECK(r.outcome == Outcome::signaled);
    CHECK(r.signal == SIGKILL);
}

TEST_CASE("output beyond max_output is dropped without blocking the program") {
    const auto r = run({.program = "head", .args = {"-c", "1000000", "/dev/zero"}, .max_output = 1000});
    CHECK(r.ok());
    CHECK(r.out.size() == 1000);
    CHECK(r.out_truncated);
    CHECK_FALSE(r.err_truncated);
}

TEST_CASE("find_executable searches PATH, then the extra directories") {
    const auto dir = std::filesystem::temp_directory_path() / ("meerkat-find-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto tool = dir / "fake-iw";
    std::ofstream{tool} << "#!/bin/sh\n";
    std::filesystem::permissions(tool, std::filesystem::perms::owner_all);
    const auto plain = dir / "not-executable";
    std::ofstream{plain} << "x";

    CHECK(find_executable("fake-iw", "/nonexistent", {dir}) == tool);  // found in extras
    CHECK(find_executable("fake-iw", dir.string(), {}) == tool);       // found in PATH
    CHECK_FALSE(find_executable("fake-iw", "/nonexistent", {}).has_value());
    CHECK_FALSE(find_executable("not-executable", dir.string(), {}).has_value());
    CHECK_FALSE(find_executable("", dir.string(), {}).has_value());
    CHECK(find_executable(tool.string(), "", {}) == tool);             // a path is checked as-is
    CHECK_FALSE(find_executable((dir / "missing").string(), "", {}).has_value());
    CHECK(find_executable("sh", "/usr/bin:/bin").has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("short runs return promptly (exit is noticed via pidfd, not a poll tick)") {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        CHECK(run({.program = "true"}).ok());
    }
    // With a 50 ms polling tick this takes >= 500 ms.
    CHECK(std::chrono::steady_clock::now() - t0 < 400ms);
}

TEST_CASE("a huge timeout doesn't overflow into an immediate timeout") {
    const auto r = run({.program = "true",
                        .timeout = std::chrono::milliseconds::max(),
                        .kill_grace = std::chrono::milliseconds::max()});
    CHECK(r.ok());
}

TEST_CASE("an empty PATH entry is not the current directory") {
    const auto dir = std::filesystem::temp_directory_path() / ("meerkat-cwd-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const auto tool = dir / "meerkat-cwd-tool";
    std::ofstream{tool} << "#!/bin/sh\n";
    std::filesystem::permissions(tool, std::filesystem::perms::owner_all);
    const auto old = std::filesystem::current_path();
    std::filesystem::current_path(dir);
    CHECK_FALSE(find_executable("meerkat-cwd-tool", ":/nonexistent:", {}).has_value());
    CHECK_FALSE(find_executable("meerkat-cwd-tool", "", {}).has_value());
    std::filesystem::current_path(old);
    std::filesystem::remove_all(dir);
}

TEST_CASE("an ignored SIGCHLD (auto-reaping) doesn't make run() throw") {
    struct sigaction ignore {}, old {};
    ignore.sa_handler = SIG_IGN;
    ::sigaction(SIGCHLD, &ignore, &old);
    ProcessResult r;
    CHECK_NOTHROW(r = run({.program = "true"}));
    ::sigaction(SIGCHLD, &old, nullptr);
    CHECK(r.outcome == Outcome::exited);
    CHECK_FALSE(r.ok());  // the exit status was lost, so success can't be claimed
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("the child gets a working stdin even if meerkat started with fd 0 closed") {
    const int saved = ::dup(STDIN_FILENO);
    REQUIRE(saved >= 0);
    ::close(STDIN_FILENO);  // /dev/null will now be opened as fd 0
    const auto r = run({.program = "cat"});
    ::dup2(saved, STDIN_FILENO);
    ::close(saved);
    CHECK(r.ok());  // without the fix, cat fails with "Bad file descriptor"
    CHECK(r.err.empty());
}

TEST_CASE("kill_running_children ends a running program and its children") {
    using namespace std::chrono_literals;
    meerkat::sys::ProcessResult result;
    // The shell starts a grandchild and waits for it: both must go.
    std::thread t([&result] {
        result = meerkat::sys::run({.program = "/bin/sh",
                                    .args = {"-c", "sleep 30 & echo $! ; wait"},
                                    .timeout = 20s});
    });
    std::this_thread::sleep_for(500ms);
    const auto start = std::chrono::steady_clock::now();
    meerkat::sys::kill_running_children();
    t.join();
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    CHECK(result.outcome == meerkat::sys::ProcessResult::Outcome::signaled);
    CHECK(result.signal == SIGKILL);
    // The grandchild's pid was printed; it must be gone, or a zombie where
    // nobody reaps orphans (PID 1 in a container may not).
    const pid_t grandchild = static_cast<pid_t>(std::stol(result.out));
    const auto dead = [grandchild] {
        if (::kill(grandchild, 0) == -1) {
            return true;
        }
        // It can be reaped between kill() and this read: the read then fails
        // with ESRCH (basic_filebuf::underflow throws; read_file() returns
        // nothing). Count it as gone only if it really is.
        const auto text =
            meerkat::core::text::read_file("/proc/" + std::to_string(grandchild) + "/stat");
        if (!text) {
            return ::kill(grandchild, 0) == -1;
        }
        const auto paren = text->rfind(')');
        return paren != std::string::npos && paren + 2 < text->size() && (*text)[paren + 2] == 'Z';
    };
    bool gone = false;
    for (int i = 0; i < 50 && !gone; ++i) {
        gone = dead();
        if (!gone) {
            std::this_thread::sleep_for(20ms);
        }
    }
    CHECK(gone);

    meerkat::sys::kill_running_children();  // nothing running: no effect
}
