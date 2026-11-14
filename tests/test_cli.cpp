/**
 * @file test_cli.cpp
 * @brief Tests for the command-line entry point.
 */
#include <doctest/doctest.h>

#include <array>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>

#include "app/cli.hpp"
#include "meerkat/version.hpp"
#include "store/pause_file.hpp"
#include "support.hpp"

TEST_CASE("--version prints the name and version") {
    const std::array<const char*, 2> args{"meerkat", "--version"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) == 0);
    CHECK(out.str() == "meerkat " + std::string{meerkat::kVersion} + "\n");
    CHECK(err.str().empty());
}

TEST_CASE("no arguments prints help and succeeds") {
    const std::array<const char*, 1> args{"meerkat"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) == 0);
    CHECK(out.str().find("--version") != std::string::npos);
    CHECK(err.str().empty());
}

TEST_CASE("--help prints help to stdout and succeeds") {
    for (const char* flag : {"-h", "--help"}) {
        CAPTURE(flag);
        const std::array<const char*, 2> args{"meerkat", flag};
        std::ostringstream out, err;
        CHECK(meerkat::app::run(args, out, err) == 0);
        CHECK(out.str().find("--version") != std::string::npos);
        CHECK(err.str().empty());
    }
}

TEST_CASE("an unexpected positional argument is a usage error") {
    const std::array<const char*, 2> args{"meerkat", "frobnicate"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) != 0);
    CHECK(err.str().find("frobnicate") != std::string::npos);
}

TEST_CASE("an unknown option is a usage error") {
    const std::array<const char*, 2> args{"meerkat", "--bogus"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) != 0);
    CHECK(err.str().find("bogus") != std::string::npos);
}

TEST_CASE("an invalid config value stops `once` before it measures anything") {
    const std::array<const char*, 4> args{"meerkat", "once", "--dns-host", "bad host"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) == 2);
    CHECK(out.str().empty());
    CHECK(err.str().find("invalid DNS host 'bad host'") != std::string::npos);
    CHECK(err.str().find("meerkat check") != std::string::npos);
}

TEST_CASE("`once` doesn't take the recording-only flags") {
    for (const char* flag : {"--contract-down", "--data-dir", "--speed-interval"}) {
        CAPTURE(flag);
        const std::array<const char*, 4> args{"meerkat", "once", flag, "x"};
        std::ostringstream out, err;
        CHECK(meerkat::app::run(args, out, err) != 0);
        CHECK(err.str().find(flag) != std::string::npos);
    }
}

TEST_CASE("`once --no-speed` can't be combined with --server-id") {
    const std::array<const char*, 5> args{"meerkat", "once", "--no-speed", "--server-id", "7"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) != 0);
    CHECK(err.str().find("excludes") != std::string::npos);
}

TEST_CASE("`check` lists a missing --config file as a config failure (exit 2)") {
    const std::array<const char*, 4> args{"meerkat", "check", "--config",
                                          "/nonexistent/meerkat.toml"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) == 2);
    CHECK(out.str().find("[FAIL] config     can't read config file /nonexistent/meerkat.toml") !=
          std::string::npos);
    // With no usable file, the data directory isn't looked at (nothing is
    // created or probed in the user's real one).
    CHECK(out.str().find("[warn] data       not checked") != std::string::npos);
    CHECK(out.str().find("problem(s) to fix before recording") != std::string::npos);
}

TEST_CASE("an invalid config value stops `run` before it records anything") {
    const std::array<const char*, 6> args{"meerkat", "run", "--speed-interval", "10s",
                                          "--data-dir", "/nonexistent/meerkat-test"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) == 2);
    CHECK(err.str().find("between 1m and 24h") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists("/nonexistent/meerkat-test"));
}

TEST_CASE("`run --help` lists the recording flags") {
    const std::array<const char*, 3> args{"meerkat", "run", "--help"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) == 0);
    CHECK(out.str().find("--data-dir") != std::string::npos);
    CHECK(out.str().find("--connectivity-interval") != std::string::npos);
}

TEST_CASE("`pause --for` writes the pause file, `resume` removes it (2.10)") {
    meerkat::test::TempDir dir;
    const auto config = (dir.path / "config.toml").string();
    std::ofstream{config} << "";  // the user's own config stays out of it
    const auto data = (dir.path / "data").string();
    std::filesystem::create_directories(data);
    std::ostringstream out, err;

    const std::array<const char*, 8> pause{"meerkat", "pause", "--for", "2h", "--config",
                                           config.c_str(), "--data-dir", data.c_str()};
    const auto before = meerkat::core::now();
    REQUIRE(meerkat::app::run(pause, out, err) == 0);
    const auto until = meerkat::store::read_pause(data);
    REQUIRE(until);
    CHECK(*until - before > std::chrono::minutes{119});
    CHECK(*until - before <= std::chrono::hours{2});
    CHECK(out.str().find("Speed tests pause until " + meerkat::core::to_iso8601(*until)) == 0);

    const std::array<const char*, 6> resume{"meerkat", "resume", "--config", config.c_str(),
                                            "--data-dir", data.c_str()};
    out.str("");
    CHECK(meerkat::app::run(resume, out, err) == 0);
    CHECK(out.str() == "Pause ended; speed tests resume at the next slot.\n");
    CHECK_FALSE(meerkat::store::read_pause(data));
    out.str("");
    CHECK(meerkat::app::run(resume, out, err) == 0);
    CHECK(out.str() == "No pause was set.\n");

    SUBCASE("a length out of range or not a duration") {
        for (const char* bad : {"30s", "73h", "soon"}) {
            CAPTURE(bad);
            const std::array<const char*, 8> args{"meerkat", "pause", "--for", bad, "--config",
                                                  config.c_str(), "--data-dir", data.c_str()};
            err.str("");
            CHECK(meerkat::app::run(args, out, err) == 2);
            CHECK(err.str().find("--for must be a duration from 1m to 72h") != std::string::npos);
        }
        CHECK_FALSE(meerkat::store::read_pause(data));
    }
    SUBCASE("no data directory") {
        const auto missing = (dir.path / "missing").string();
        const std::array<const char*, 8> args{"meerkat", "pause", "--for", "1h", "--config",
                                              config.c_str(), "--data-dir", missing.c_str()};
        err.str("");
        CHECK(meerkat::app::run(args, out, err) == 1);
        CHECK(err.str().find("no data directory") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(missing));
        const std::array<const char*, 6> again{"meerkat", "resume", "--config", config.c_str(),
                                               "--data-dir", missing.c_str()};
        err.str("");
        out.str("");
        CHECK(meerkat::app::run(again, out, err) == 1);  // not "No pause was set."
        CHECK(out.str().empty());
    }
    SUBCASE("--for is required") {
        const std::array<const char*, 2> args{"meerkat", "pause"};
        CHECK(meerkat::app::run(args, out, err) != 0);
    }
}
