/**
 * @file test_config.cpp
 * @brief Tests for core::Config: parsing, defaults and validation.
 */
#include <doctest/doctest.h>

#include <map>
#include <string>

#include "core/config.hpp"
#include "support.hpp"

using namespace meerkat::core;
using namespace std::chrono_literals;

namespace {

/// An environment with exactly @p vars.
EnvLookup env_of(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string{name});
        if (it == vars.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

/// Defaults for a user whose home is /home/user.
Config base() { return default_config(env_of({{"HOME", "/home/user"}})); }

/// Parses @p text on top of base().
Config parse(const std::string& text) {
    return parse_config(text, "test.toml", base(), env_of({{"HOME", "/home/user"}}));
}

/// The message of the ConfigError that parse(@p text) throws.
std::string error_of(const std::string& text) {
    try {
        parse(text);
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "(no error)";
}

/// True if @p haystack contains @p needle.
bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("parse_duration and format_duration") {
    CHECK(parse_duration("30") == 30s);
    CHECK(parse_duration("30s") == 30s);
    CHECK(parse_duration("5m") == 300s);
    CHECK(parse_duration("1h") == 3600s);
    CHECK(parse_duration("0") == 0s);
    CHECK_FALSE(parse_duration(""));
    CHECK_FALSE(parse_duration("m"));
    CHECK_FALSE(parse_duration("-5m"));
    CHECK_FALSE(parse_duration("5 m"));
    CHECK_FALSE(parse_duration("1.5h"));
    CHECK_FALSE(parse_duration("5d"));
    CHECK_FALSE(parse_duration("99999999999999999h"));
    CHECK_FALSE(parse_duration("169h"));  // more than a week

    CHECK(format_duration(300s) == "5m");
    CHECK(format_duration(30s) == "30s");
    CHECK(format_duration(7200s) == "2h");
    CHECK(format_duration(90s) == "90s");
    CHECK(format_duration(0s) == "0s");
}

TEST_CASE("parse_contract_speeds") {
    CHECK(parse_contract_speeds("250/200/150") == ContractSpeeds{250, 200, 150});
    CHECK(parse_contract_speeds("50/40/30") == ContractSpeeds{50, 40, 30});
    CHECK(parse_contract_speeds("27.5 / 20 / 12.5") == ContractSpeeds{27.5, 20, 12.5});
    for (const char* bad : {"", "250", "250/200", "250/200/150/100", "250,200,150", "a/b/c",
                            "250//150", "250/200/150/", "250.5.1/2/1", "250 200 150"}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(parse_contract_speeds(bad), ConfigError);
    }
}

TEST_CASE("expand_home") {
    const auto env = env_of({{"HOME", "/home/user"}});
    CHECK(expand_home("~/data", env) == "/home/user/data");
    CHECK(expand_home("~", env) == "/home/user/");
    CHECK(expand_home("/srv/meerkat", env) == "/srv/meerkat");
    CHECK(expand_home("rel/path", env) == "rel/path");
    CHECK_THROWS_AS(expand_home("~other/x", env), ConfigError);
    CHECK_THROWS_AS(expand_home("~/x", env_of({})), ConfigError);
}

TEST_CASE("default_config picks the data directory") {
    CHECK(default_config(env_of({{"STATE_DIRECTORY", "/var/lib/meerkat"}, {"HOME", "/h"}})).data_dir ==
          "/var/lib/meerkat");
    CHECK(default_config(env_of({{"STATE_DIRECTORY", "/var/lib/a:/var/lib/b"}})).data_dir ==
          "/var/lib/a");
    CHECK(default_config(env_of({{"XDG_DATA_HOME", "/x"}, {"HOME", "/h"}})).data_dir == "/x/meerkat");
    CHECK(default_config(env_of({{"XDG_DATA_HOME", "relative"}, {"HOME", "/h"}})).data_dir ==
          "/h/.local/share/meerkat");  // XDG says: ignore relative paths
    CHECK(default_config(env_of({{"HOME", "/h"}})).data_dir == "/h/.local/share/meerkat");
    CHECK(default_config(env_of({})).data_dir == "/var/lib/meerkat");
    // Empty values count as unset.
    CHECK(default_config(env_of({{"STATE_DIRECTORY", ""}, {"XDG_DATA_HOME", ""}, {"HOME", "/h"}}))
              .data_dir == "/h/.local/share/meerkat");
    CHECK(default_config(env_of({{"HOME", ""}})).data_dir == "/var/lib/meerkat");

    const auto d = default_config(env_of({{"HOME", "/h"}}));
    CHECK(d.speed_interval == 5min);
    CHECK(d.connectivity_interval == 30s);
    CHECK_FALSE(d.contract.download.has_value());  // never a built-in contract
    CHECK_FALSE(d.contract.upload.has_value());
    CHECK(validate(d).empty());
}

TEST_CASE("config_search_path") {
    CHECK(config_search_path(env_of({{"HOME", "/h"}})) ==
          std::vector<std::filesystem::path>{"/h/.config/meerkat/config.toml",
                                             "/etc/meerkat/config.toml"});
    CHECK(config_search_path(env_of({{"XDG_CONFIG_HOME", "/c"}, {"HOME", "/h"}})).front() ==
          "/c/meerkat/config.toml");
    CHECK(config_search_path(env_of({})) ==
          std::vector<std::filesystem::path>{"/etc/meerkat/config.toml"});
    CHECK(config_search_path(env_of({{"XDG_CONFIG_HOME", ""}, {"HOME", ""}})) ==
          std::vector<std::filesystem::path>{"/etc/meerkat/config.toml"});
}

TEST_CASE("parse_config reads every setting") {
    const auto c = parse(R"(
data_dir = "~/meerkat-data"
[speed]
interval = "10m"
backend = "ookla"
program = "/opt/ookla/speedtest"
server_id = 12345
require_wired = true
[connectivity]
interval = 15
ping_hosts = ["192.0.2.1", "example.org"]
ping_gateway = false
dns_host = "example.net"
outage_after = 4
[contract]
download = { maximum = 250, normal = 200, minimum = 150 }
upload = { maximum = 50.5, normal = 40, minimum = 30 }
)");
    CHECK(c.data_dir == "/home/user/meerkat-data");
    CHECK(c.speed_interval == 10min);
    CHECK(c.speedtest == "/opt/ookla/speedtest");
    CHECK(c.server_id == 12345);
    CHECK(c.require_wired);
    CHECK(c.connectivity_interval == 15s);
    CHECK(c.ping_hosts == std::vector<std::string>{"192.0.2.1", "example.org"});
    CHECK_FALSE(c.ping_gateway);
    CHECK(c.dns_host == "example.net");
    CHECK(c.outage_after == 4);
    REQUIRE(c.contract.download);
    CHECK(*c.contract.download == ContractSpeeds{250, 200, 150});
    REQUIRE(c.contract.upload);
    CHECK(*c.contract.upload == ContractSpeeds{50.5, 40, 30});
    CHECK(validate(c).empty());
}

TEST_CASE("parse_config keeps the base values for missing settings") {
    const auto c = parse("[speed]\nserver_id = 7\n");
    CHECK(c.server_id == 7);
    CHECK(c.speed_interval == 5min);
    CHECK(c.data_dir == "/home/user/.local/share/meerkat");
    CHECK(parse("").ping_hosts == base().ping_hosts);
}

TEST_CASE("parse_config errors point at the line") {
    CHECK(has(error_of("[speed]\ninterval = \"5 minutes\"\n"), "test.toml:2:"));
    CHECK(has(error_of("[speed]\ninterval = \"5 minutes\"\n"), "'speed.interval' must be a duration"));
    CHECK(has(error_of("[speed]\nintervall = \"5m\"\n"), "unknown setting 'speed.intervall'"));
    CHECK(has(error_of("[speeed]\n"), "unknown setting 'speeed'"));
    CHECK(has(error_of("[speed]\nserver_id = \"12345\"\n"), "'speed.server_id' must be a whole number"));
    CHECK(has(error_of("[speed]\nrequire_wired = 1\n"), "true or false"));
    CHECK(has(error_of("[connectivity]\nping_hosts = \"1.1.1.1\"\n"), "a list of strings"));
    CHECK(has(error_of("[connectivity]\nping_hosts = [1]\n"), "a list of strings"));
    CHECK(has(error_of("data_dir = \"relative\"\n"), "absolute path"));
    CHECK(has(error_of("data_dir = \"~bob/x\"\n"), "only ~/"));
    CHECK(has(error_of("speed = 5\n"), "'speed' must be a table"));
    CHECK(has(error_of("[contract]\ndownload = { maximum = 250, normal = 200 }\n"),
              "'contract.download.minimum' is missing"));
    CHECK(has(error_of("[contract]\ndownload = { max = 250, normal = 200, minimum = 1 }\n"),
              "unknown setting 'contract.download.max'"));
    CHECK(has(error_of("[contract]\nupload = { maximum = \"50\", normal = 40, minimum = 30 }\n"),
              "a number"));
    CHECK(has(error_of("[speed\n"), "test.toml:1:"));  // syntax error
}

TEST_CASE("validate") {
    Config c = base();
    REQUIRE(validate(c).empty());
    const auto one = [](const Config& x, const std::string& needle) {
        const auto e = validate(x);
        CAPTURE(e.size());
        return e.size() == 1 && has(e.front(), needle);
    };

    SUBCASE("data_dir") {
        c.data_dir = "relative";
        CHECK(one(c, "absolute"));
    }
    SUBCASE("intervals") {
        c.speed_interval = 30s;
        CHECK(one(c, "between 1m and 24h"));
        c.speed_interval = 25h;
        CHECK(one(c, "between 1m and 24h"));
        c.speed_interval = 5min;
        c.connectivity_interval = 2s;
        CHECK(one(c, "between 5s and 1h"));
        c.speed_interval = 2min;
        c.connectivity_interval = 3min;
        CHECK(one(c, "must not be longer"));
    }
    SUBCASE("backend, program, server") {
        c.backend = "librespeed";
        CHECK(one(c, "unknown speed-test backend"));
        c.backend = "ookla";
        c.speedtest.clear();
        CHECK(one(c, "program"));
        c.speedtest = "speedtest";
        c.server_id = 0;
        CHECK(one(c, "positive"));
    }
    SUBCASE("ping hosts and DNS") {
        c.ping_hosts = {"-f"};
        CHECK(one(c, "invalid ping host '-f'"));
        c.ping_hosts = std::vector<std::string>(11, "192.0.2.1");
        CHECK(one(c, "at most 10"));
        c.ping_hosts.clear();
        CHECK(validate(c).empty());  // the gateway alone is fine
        c.ping_gateway = false;
        CHECK(one(c, "nothing to ping"));
        c.ping_gateway = true;
        c.dns_host = "exa mple.com";
        CHECK(one(c, "invalid DNS host"));
    }
    SUBCASE("outage_after") {
        c.outage_after = 1;
        CHECK(validate(c).empty());
        c.outage_after = 20;
        CHECK(validate(c).empty());
        c.outage_after = 0;
        CHECK(one(c, "outage_after must be between 1 and 20 rounds (got 0)"));
        c.outage_after = 21;
        CHECK(one(c, "between 1 and 20"));
    }
    SUBCASE("contract") {
        c.contract.download = ContractSpeeds{250, 200, 150};
        c.contract.upload = ContractSpeeds{50, 40, 30};
        CHECK(validate(c).empty());
        c.contract.download = ContractSpeeds{250, 200, 200};  // normal = minimum is allowed
        CHECK(validate(c).empty());
        c.contract.download = ContractSpeeds{150, 200, 250};
        CHECK(one(c, "maximum ≥ normal ≥ minimum"));
        c.contract.download = ContractSpeeds{250, 200, 0};
        CHECK(one(c, "between 0 and 100000"));
        c.contract.download = ContractSpeeds{200'000, 200, 150};
        CHECK(one(c, "between 0 and 100000"));
        c.contract.download.reset();
        c.contract.upload = ContractSpeeds{-50, 40, 30};
        CHECK(one(c, "contract upload"));
    }
}

TEST_CASE("config.example.toml in the repository is valid and matches the defaults") {
    const auto path = meerkat::test::fixture_path("../../config.example.toml");
    const auto c = load_config_file(path, base(), env_of({{"HOME", "/home/user"}}));
    CHECK(validate(c).empty());
    const auto d = base();
    CHECK(c.data_dir == d.data_dir);
    CHECK(c.speed_interval == d.speed_interval);
    CHECK(c.connectivity_interval == d.connectivity_interval);
    CHECK(c.speedtest == d.speedtest);
    CHECK(c.ping_hosts == d.ping_hosts);
    CHECK(c.ping_gateway == d.ping_gateway);
    CHECK(c.dns_host == d.dns_host);
    CHECK(c.outage_after == d.outage_after);
    CHECK(c.require_wired == d.require_wired);
    CHECK_FALSE(c.server_id.has_value());
    CHECK_FALSE(c.contract.download.has_value());  // the example tariff is commented out

    SUBCASE("with the example contract uncommented") {
        auto text = meerkat::core::text::read_file(path).value();
        for (const std::string line : {"# [contract]", "# download", "# upload"}) {
            const auto pos = text.find("\n" + line);
            REQUIRE(pos != std::string::npos);
            text.erase(pos + 1, 2);
        }
        const auto k = parse(text).contract;
        REQUIRE(k.download);
        CHECK(*k.download == ContractSpeeds{250, 200, 150});
        REQUIRE(k.upload);
        CHECK(*k.upload == ContractSpeeds{50, 40, 30});
    }
}

TEST_CASE("load_config_file reports an unreadable file") {
    CHECK_THROWS_AS(load_config_file("/nonexistent/meerkat.toml", base()), ConfigError);
}
