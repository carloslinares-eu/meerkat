/**
 * @file support.hpp
 * @brief Test helpers: fixture loading and a fake process runner.
 */
#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/text.hpp"
#include "sys/process.hpp"

/**
 * @namespace meerkat::test
 * @brief Helpers shared by the unit tests.
 */
namespace meerkat::test {

/**
 * @brief Returns the path of a file or directory in tests/fixtures.
 * @param relative Path below tests/fixtures, e.g. `"sys"`.
 * @return The full path.
 */
inline std::filesystem::path fixture_path(const std::string& relative) {
    return std::filesystem::path{MEERKAT_FIXTURES_DIR} / relative;
}

/**
 * @brief Reads a file from tests/fixtures.
 * @param relative Path below tests/fixtures, e.g. `"iw/dev.txt"`.
 * @return The file's contents.
 * @throws std::runtime_error if the file can't be read.
 */
inline std::string fixture(const std::string& relative) {
    auto text = core::text::read_file(fixture_path(relative));
    if (!text) {
        throw std::runtime_error("missing fixture: " + fixture_path(relative).string());
    }
    return *text;
}

/// A fresh temporary directory, removed again (with its content) at the end.
struct TempDir {
    std::filesystem::path path;  ///< The directory.

    /// Creates the directory under the system temp directory.
    TempDir() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "meerkat-test-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        path = tmpl;
    }

    /// Removes the directory; errors are ignored.
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;             ///< Owns the directory: not copyable.
    TempDir& operator=(const TempDir&) = delete;  ///< Owns the directory: not copyable.
};

/// A sys::Runner that returns a canned result and remembers every command.
struct FakeRunner {
    sys::ProcessResult result;          ///< What every run returns.
    std::vector<sys::Command> calls{};  ///< Commands it was asked to run.

    /**
     * @brief A result for a program that exited with @p code and printed @p out / @p err.
     * @param out  stdout.
     * @param code Exit code.
     * @param err  stderr.
     * @return The result.
     */
    static sys::ProcessResult exited(std::string out, int code = 0, std::string err = {}) {
        sys::ProcessResult r;
        r.outcome = sys::ProcessResult::Outcome::exited;
        r.exit_code = code;
        r.out = std::move(out);
        r.err = std::move(err);
        return r;
    }

    /**
     * @brief A result for a program that isn't installed.
     * @return The result.
     */
    static sys::ProcessResult not_found() {
        sys::ProcessResult r;
        r.outcome = sys::ProcessResult::Outcome::spawn_failed;
        r.not_found = true;
        r.error = "program not found";
        return r;
    }

    /**
     * @brief A result for a program that hit its timeout.
     * @return The result.
     */
    static sys::ProcessResult timed_out() {
        sys::ProcessResult r;
        r.outcome = sys::ProcessResult::Outcome::timed_out;
        r.signal = 15;
        return r;
    }

    /**
     * @brief Returns a sys::Runner bound to this fake.
     * @return The runner; it must not outlive the fake.
     */
    sys::Runner runner() {
        return [this](const sys::Command& c) {
            calls.push_back(c);
            return result;
        };
    }
};

}  // namespace meerkat::test
