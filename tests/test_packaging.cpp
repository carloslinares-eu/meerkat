/**
 * @file test_packaging.cpp
 * @brief Keeps the service files in packaging/ in step with each other and
 *        with the code: the user, the paths, the hardening, and the polkit
 *        actions logind checks for the lock meerkat asks for.
 */
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "core/text.hpp"
#include "support.hpp"
#include "sys/sleep_inhibitor.hpp"

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

/// @return Whether @p text has a line exactly @p line.
bool has_line(const std::string& text, std::string_view line) {
    bool found = false;
    meerkat::core::text::for_each_line(text, [&](std::string_view l) { found = found || l == line; });
    return found;
}

}  // namespace

TEST_CASE("meerkat.service: user, paths and hardening") {
    const auto unit = repo_file("packaging/meerkat.service");
    for (const auto* line : {
             "User=meerkat",
             "Group=meerkat",
             "ExecStart=/usr/local/bin/meerkat run --config /etc/meerkat/config.toml",
             "StateDirectory=meerkat",  // meerkat's default data dir comes from $STATE_DIRECTORY
             "Environment=HOME=/var/cache/meerkat",
             "CacheDirectory=meerkat",
             "UMask=0027",
             "NoNewPrivileges=yes",
             "CapabilityBoundingSet=",
             "ProtectSystem=strict",
             "ProtectHome=yes",
             "PrivateTmp=yes",
             "PrivateDevices=yes",
             "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK",
             "SystemCallFilter=@system-service",
             "WantedBy=multi-user.target",
         }) {
        CAPTURE(line);
        CHECK(has_line(unit, line));
    }
    // install.sh rewrites the binary's path with --prefix: its pattern must match.
    const auto install = repo_file("packaging/install.sh");
    CHECK(install.find("s|^ExecStart=/usr/local/bin/meerkat |ExecStart=$bin_dest |") != std::string::npos);
    CHECK(install.find("prefix=/usr/local\n") != std::string::npos);
    CHECK(install.find("config_dest=\"$etc_dir/config.toml\"") != std::string::npos);
    CHECK(install.find("etc_dir=/etc/meerkat\n") != std::string::npos);
}

TEST_CASE("the sysusers file creates the unit's user") {
    const auto sysusers = repo_file("packaging/meerkat.sysusers");
    CHECK(sysusers.find("\nu meerkat - ") != std::string::npos);
    CHECK(sysusers.find("/usr/sbin/nologin") != std::string::npos);
}

TEST_CASE("the polkit rule allows exactly the locks meerkat asks for, to its user") {
    const auto rules = repo_file("packaging/50-meerkat.rules");
    CHECK(rules.find("subject.user == \"meerkat\"") != std::string::npos);
    // InhibitRequest::what, e.g. sleep:idle:handle-lid-switch → the
    // inhibit-block-sleep, inhibit-block-idle and inhibit-handle-lid-switch actions.
    const meerkat::sys::InhibitRequest request;
    std::string_view what = request.what;
    std::size_t actions = 0;
    while (!what.empty()) {
        const auto colon = what.find(':');
        const auto item = std::string{what.substr(0, colon)};
        const auto action = "org.freedesktop.login1.inhibit-" +
                            (item.starts_with("handle-") ? item : request.mode + "-" + item);
        CAPTURE(action);
        CHECK(rules.find("\"" + action + "\"") != std::string::npos);
        ++actions;
        what = colon == std::string_view::npos ? std::string_view{} : what.substr(colon + 1);
    }
    CHECK(actions == 3);
    std::size_t listed = 0;
    for (auto at = rules.find("action.id =="); at != std::string::npos; at = rules.find("action.id ==", at + 1)) {
        ++listed;
    }
    CHECK(listed == actions);  // nothing more
}
