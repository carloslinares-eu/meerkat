/**
 * @file test_packaging.cpp
 * @brief Keeps the service files in packaging/ in step with each other and
 *        with the code: the user, the paths, the hardening, and the polkit
 *        actions logind checks for the lock meerkat asks for.
 */
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>

#include "core/text.hpp"
#include "support.hpp"
#include "sys/sleep_inhibitor.hpp"

namespace {

/**
 * @brief A file of the repository.
 * @param relative Path from the repository root.
 * @return Its content.
 * @throws std::runtime_error if it can't be read.
 */
std::string repo_file(const std::string& relative) { return meerkat::test::fixture("../../" + relative); }

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
             "StartLimitIntervalSec=10min",
             "StartLimitBurst=5",
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
    // The post-install check copies the hardening block by its first and last lines.
    CHECK(install.find("/^NoNewPrivileges=/,/^SystemCallErrorNumber=/") != std::string::npos);
    const auto first = unit.find("\nNoNewPrivileges=");
    const auto last = unit.find("\nSystemCallErrorNumber=");
    REQUIRE(first != std::string::npos);
    REQUIRE(last != std::string::npos);
    CHECK(first < unit.find("\nProtectHome=yes"));
    CHECK(unit.find("\nProtectHome=yes") < last);
    // --speedtest lands on the service's PATH; the program comes from [speed].
    CHECK(install.find("speedtest_dest=/usr/local/bin/speedtest\n") != std::string::npos);
    CHECK(install.find("service_path=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin\n") != std::string::npos);
    CHECK(install.find("section == \"speed\" && /^[[:space:]]*program") != std::string::npos);
    CHECK(has_line(repo_file("config.example.toml"), "[speed]"));
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

TEST_CASE("the .deb's files agree with each other and with the notices (4.2)") {
    const auto pkg = repo_file("cmake/Package.cmake");
    const auto postinst = repo_file("packaging/debian/postinst");
    // postinst names the files Package.cmake installs (with the /usr prefix).
    CHECK(pkg.find("install(FILES packaging/meerkat.sysusers DESTINATION lib/sysusers.d RENAME meerkat.conf)") !=
          std::string::npos);
    CHECK(postinst.find("systemd-sysusers /usr/lib/sysusers.d/meerkat.conf") != std::string::npos);
    CHECK(pkg.find("${CMAKE_INSTALL_FULL_SYSCONFDIR}/meerkat RENAME config.toml") != std::string::npos);
    CHECK(has_line(repo_file("packaging/debian/conffiles"), "/etc/meerkat/config.toml"));
    CHECK(postinst.find("\"0640 /etc/meerkat/config.toml\"") != std::string::npos);
    CHECK(postinst.find("dpkg-statoverride --update --add root meerkat") != std::string::npos);
    // Never enabled by the package: the user enables it once the config is ready (#116).
    meerkat::core::text::for_each_line(postinst, [](std::string_view line) {
        CHECK_FALSE(meerkat::core::text::trim(line).starts_with("systemctl enable"));  // the message may say it
    });
    CHECK(pkg.find("option(MEERKAT_INSTALL_SERVICE") != std::string::npos);
    CHECK(repo_file("CMakePresets.json").find("\"MEERKAT_INSTALL_SERVICE\": \"ON\"") != std::string::npos);
    // Package.cmake rewrites install.sh's ExecStart= line; it must be there.
    CHECK(pkg.find("set(meerkat_exec_line \"ExecStart=/usr/local/bin/meerkat run --config /etc/meerkat/config.toml\")") !=
          std::string::npos);
    CHECK(has_line(repo_file("packaging/meerkat.service"),
                   "ExecStart=/usr/local/bin/meerkat run --config /etc/meerkat/config.toml"));
    // The evidence survives a purge: no script does anything to the data
    // directory but name it in a comment or a message.
    for (const auto* script : {"postinst", "prerm", "postrm"}) {
        CAPTURE(script);
        meerkat::core::text::for_each_line(repo_file(std::string{"packaging/debian/"} + script),
                                           [](std::string_view line) {
                                               if (line.find("/var/lib/meerkat") != std::string_view::npos) {
                                                   const auto t = meerkat::core::text::trim(line);
                                                   CHECK((t.starts_with("#") || t.starts_with("echo ")));
                                               }
                                           });
    }

    // Debian's copyright file carries every copyright line and licence of
    // THIRD_PARTY_LICENSES.md, and the same versions as cmake/Deps.cmake.
    const auto copyright = repo_file("packaging/debian/copyright");
    const auto notices = repo_file("THIRD_PARTY_LICENSES.md");
    CHECK(copyright.starts_with("Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n"));
    for (const auto* holder : {"2013-2025 Niels Lohmann", "Mark Gillard <mark.gillard@outlook.com.au>",
                               "2017-2025 University of Cincinnati"}) {
        CAPTURE(holder);
        CHECK(notices.find(holder) != std::string::npos);
        CHECK(copyright.find("Copyright: " + std::string{holder}) != std::string::npos);
    }
    const auto deps = repo_file("cmake/Deps.cmake");
    for (const auto& [lib, version] : {std::pair{"CLI11", "2.5.0"}, std::pair{"nlohmann/json", "3.12.0"},
                                       std::pair{"toml++", "3.4.0"}}) {
        CAPTURE(lib);
        CHECK(copyright.find("Comment: " + std::string{lib} + " " + version + ",") != std::string::npos);
        CHECK(deps.find(std::string{"v"} + version) != std::string::npos);
    }
    CHECK(has_line(copyright, "License: MIT"));
    CHECK(has_line(copyright, "License: BSD-3-Clause"));
    CHECK(copyright.find(" 3. Neither the name of the copyright holder") != std::string::npos);
}
