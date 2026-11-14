/**
 * @file test_dir_lock.cpp
 * @brief Tests for store::DirectoryLock.
 */
#include <doctest/doctest.h>

#include <fcntl.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "store/dir_lock.hpp"
#include "support.hpp"

using meerkat::store::DirectoryLock;
using meerkat::test::TempDir;
namespace fs = std::filesystem;

namespace {

/**
 * @brief The descriptors this process has open on @p dir.
 * @param dir A directory.
 * @return For each one, whether it is close-on-exec.
 */
std::vector<bool> open_on(const fs::path& dir) {
    const auto target = fs::canonical(dir);
    std::vector<bool> cloexec;
    for (const auto& e : fs::directory_iterator{"/proc/self/fd"}) {
        std::error_code ec;
        if (fs::read_symlink(e.path(), ec) != target || ec) {
            continue;
        }
        const int fd = std::stoi(e.path().filename().string());
        cloexec.push_back((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    }
    return cloexec;
}

}  // namespace

TEST_CASE("DirectoryLock: one holder at a time, until it's destroyed") {
    TempDir dir;
    {
        const DirectoryLock first{dir.path};
        CHECK(first.state() == DirectoryLock::State::held);
        CHECK(first.error().empty());
        // flock() locks belong to the open file, so a second lock in this
        // process is refused just like one in another process.
        const DirectoryLock second{dir.path};
        CHECK(second.state() == DirectoryLock::State::busy);
        CHECK(second.error().empty());
        CHECK(fs::is_empty(dir.path));  // nothing is left in the directory
    }
    CHECK(DirectoryLock{dir.path}.state() == DirectoryLock::State::held);  // released
}

TEST_CASE("DirectoryLock: close-on-exec, so ping or speedtest can't keep it alive") {
    TempDir dir;
    CHECK(open_on(dir.path).empty());
    {
        const DirectoryLock lock{dir.path};
        REQUIRE(lock.state() == DirectoryLock::State::held);
        const DirectoryLock refused{dir.path};
        REQUIRE(refused.state() == DirectoryLock::State::busy);
        CHECK(open_on(dir.path) == std::vector<bool>{true});  // refused keeps nothing open
    }
    CHECK(open_on(dir.path).empty());
}

TEST_CASE("DirectoryLock: something that isn't a directory can't be locked") {
    TempDir dir;
    CHECK_THROWS_AS(DirectoryLock{dir.path / "missing"}, std::system_error);
    std::ofstream{dir.path / "file"} << "x";
    try {
        const DirectoryLock lock{dir.path / "file"};
        FAIL("a file was locked");
    } catch (const std::system_error& e) {
        CHECK(e.code() == std::errc::not_a_directory);
        CHECK(std::string{e.what()}.starts_with("can't open " + (dir.path / "file").string()));
    }
}

TEST_CASE("DirectoryLock: moving hands the lock over") {
    TempDir dir;
    DirectoryLock a{dir.path};
    REQUIRE(a.state() == DirectoryLock::State::held);
    DirectoryLock b{std::move(a)};
    CHECK(b.state() == DirectoryLock::State::held);
    CHECK(a.state() == DirectoryLock::State::released);  // NOLINT(bugprone-use-after-move)
    CHECK(a.error().empty());                            // NOLINT(bugprone-use-after-move)
    CHECK(DirectoryLock{dir.path}.state() == DirectoryLock::State::busy);  // b holds it

    TempDir other;
    DirectoryLock c{other.path};
    REQUIRE(c.state() == DirectoryLock::State::held);
    c = std::move(b);  // lets go of other, takes over dir
    CHECK(c.state() == DirectoryLock::State::held);
    CHECK(DirectoryLock{other.path}.state() == DirectoryLock::State::held);
    CHECK(DirectoryLock{dir.path}.state() == DirectoryLock::State::busy);
    CHECK(b.state() == DirectoryLock::State::released);  // NOLINT(bugprone-use-after-move)
    CHECK(open_on(dir.path).size() == 1);
    CHECK(open_on(other.path).empty());
}

TEST_CASE("DirectoryLock: with patience, waits for a holder that lets go (meerkat check)") {
    TempDir dir;
    auto holder = std::make_unique<DirectoryLock>(dir.path);
    REQUIRE(holder->state() == DirectoryLock::State::held);
    std::thread lets_go{[&holder] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        holder.reset();
    }};
    const DirectoryLock patient{dir.path, std::chrono::seconds{10}};  // returns when it gets it
    lets_go.join();
    CHECK(patient.state() == DirectoryLock::State::held);
}

TEST_CASE("DirectoryLock: patience runs out while the holder keeps it") {
    TempDir dir;
    const DirectoryLock holder{dir.path};
    const auto start = std::chrono::steady_clock::now();
    const DirectoryLock patient{dir.path, std::chrono::milliseconds{50}};
    CHECK(patient.state() == DirectoryLock::State::busy);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{50});
    CHECK(open_on(dir.path).size() == 1);  // the holder's only
}
