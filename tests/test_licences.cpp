/**
 * @file test_licences.cpp
 * @brief Keeps the licence notices in step with the dependencies: every
 *        library fetched in cmake/Deps.cmake is listed, at its version, in
 *        THIRD_PARTY_LICENSES.md (if it is compiled in) and in the README.
 */
#include <doctest/doctest.h>

#include <map>
#include <regex>
#include <string>

#include "core/text.hpp"
#include "support.hpp"

namespace {

/**
 * @brief A file of the repository.
 * @param relative Path from the repository root.
 * @return Its content; fails the test if it can't be read.
 */
std::string repo_file(const std::string& relative) {
    const auto text = meerkat::core::text::read_file(meerkat::test::fixture_path("../../" + relative));
    REQUIRE_MESSAGE(text, "can't read " << relative);
    return *text;
}

/**
 * @brief The version of each FetchContent_Declare in @p deps, from its
 *        GIT_TAG (`v2.5.0`) or its URL (`.../v3.12.0/...`).
 * @param deps The content of cmake/Deps.cmake.
 * @return Name → version without the `v`.
 */
std::map<std::string, std::string> fetched_versions(const std::string& deps) {
    std::map<std::string, std::string> out;
    const std::regex declare{R"(FetchContent_Declare\((\w+)([^)]*)\))"};
    const std::regex version{R"((?:GIT_TAG\s+v|/v)(\d+\.\d+\.\d+))"};
    for (auto it = std::sregex_iterator(deps.begin(), deps.end(), declare);
         it != std::sregex_iterator{}; ++it) {
        const std::string body = (*it)[2];
        std::smatch m;
        REQUIRE_MESSAGE(std::regex_search(body, m, version), "no version for " << (*it)[1]);
        out[(*it)[1]] = m[1];
    }
    return out;
}

}  // namespace

TEST_CASE("licence notices list every fetched dependency at its version") {
    const auto deps = fetched_versions(repo_file("cmake/Deps.cmake"));
    REQUIRE(deps.size() == 4);
    const auto notice = repo_file("THIRD_PARTY_LICENSES.md");
    const auto readme = repo_file("README.md");

    // Name in cmake → how the documents name it.
    const std::map<std::string, std::string> shown{
        {"cli11", "CLI11"}, {"nlohmann_json", "nlohmann/json"},
        {"tomlplusplus", "toml++"}, {"doctest", "doctest"}};
    for (const auto& [name, version] : deps) {
        CAPTURE(name);
        CAPTURE(version);
        REQUIRE(shown.count(name) == 1);  // a new dependency: add it to the notices
        const auto& title = shown.at(name);
        CHECK(readme.find(title) != std::string::npos);
        CHECK(readme.find(version) != std::string::npos);
        if (name == "doctest") {
            continue;  // tests only: not compiled into meerkat, no notice
        }
        // The table row and the heading of the licence text.
        CHECK(notice.find("| " + version + " |") != std::string::npos);
        CHECK(notice.find("## " + title + " " + version + " (") != std::string::npos);
    }
}
