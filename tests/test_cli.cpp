/**
 * @file test_cli.cpp
 * @brief Tests for the command-line entry point.
 */
#include <doctest/doctest.h>

#include <array>
#include <sstream>
#include <string>

#include "app/cli.hpp"
#include "meerkat/version.hpp"

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

TEST_CASE("an invalid config flag stops `once` before it measures anything") {
    const std::array<const char*, 4> args{"meerkat", "once", "--contract-down", "250/200"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) == 2);
    CHECK(out.str().empty());
    CHECK(err.str().find("--contract-down: contract speeds must be MAX/NORMAL/MIN") !=
          std::string::npos);
    CHECK(err.str().find("meerkat check") != std::string::npos);
}

TEST_CASE("`once --no-speed` can't be combined with --server-id") {
    const std::array<const char*, 5> args{"meerkat", "once", "--no-speed", "--server-id", "7"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) != 0);
    CHECK(err.str().find("excludes") != std::string::npos);
}

TEST_CASE("`check` lists a missing --config file as a failure") {
    const std::array<const char*, 4> args{"meerkat", "check", "--config",
                                          "/nonexistent/meerkat.toml"};
    std::ostringstream out, err;
    CHECK(meerkat::app::run(args, out, err) == 1);
    CHECK(out.str().find("[FAIL] config     can't read config file /nonexistent/meerkat.toml") !=
          std::string::npos);
    CHECK(out.str().find("problem(s) to fix before recording") != std::string::npos);
}
