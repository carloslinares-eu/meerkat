/**
 * @file test_ookla.cpp
 * @brief Tests for the Ookla speed-test backend.
 */
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "probe/ookla.hpp"
#include "support.hpp"

using meerkat::core::Status;
using meerkat::probe::OoklaBackend;
using meerkat::probe::OoklaOptions;
using meerkat::probe::ookla_errors;
using meerkat::probe::parse_ookla;
using meerkat::probe::SpeedBackend;
using meerkat::test::fixture;
using meerkat::test::FakeRunner;
using namespace std::chrono_literals;

TEST_CASE("parse_ookla: full result (bytes/s converted to Mbit/s)") {
    const auto text = fixture("speedtest/ookla_ok.json");
    const auto r = parse_ookla(text);
    CHECK(r.status == Status::ok);
    CHECK(r.error.empty());
    CHECK(r.backend == "ookla");
    REQUIRE(r.download_mbps.has_value());
    CHECK(*r.download_mbps == doctest::Approx(50.0));  // 6 250 000 B/s
    REQUIRE(r.upload_mbps.has_value());
    CHECK(*r.upload_mbps == doctest::Approx(25.0));    // 3 125 000 B/s
    CHECK(r.latency_ms == doctest::Approx(12.375));
    CHECK(r.jitter_ms == doctest::Approx(0.512));
    CHECK(r.packet_loss_percent == doctest::Approx(0.0));
    CHECK(r.server_id == 12345);
    CHECK(r.server_name == "Example Server");
    CHECK(r.server_location == "Example City");
    CHECK(r.isp == "Example ISP");
    CHECK(r.external_ip == "2001:db8:1::10");
    CHECK(r.result_url == "https://www.speedtest.net/result/c/00000000-0000-4000-8000-000000000000");
    CHECK(r.raw == std::string{meerkat::core::text::trim(text)});
}

TEST_CASE("parse_ookla: missing fields stay empty, never 0") {
    const auto r = parse_ookla(R"({"type":"result","download":{"bandwidth":1000000},"upload":{"bandwidth":500000}})");
    CHECK(r.status == Status::ok);
    CHECK(*r.download_mbps == doctest::Approx(8.0));
    CHECK(*r.upload_mbps == doctest::Approx(4.0));
    CHECK_FALSE(r.latency_ms.has_value());
    CHECK_FALSE(r.jitter_ms.has_value());
    CHECK_FALSE(r.packet_loss_percent.has_value());  // Ookla omits it when it can't measure loss
    CHECK_FALSE(r.server_id.has_value());
    CHECK(r.isp.empty());
}

TEST_CASE("parse_ookla: unusable output") {
    SUBCASE("empty") { CHECK(parse_ookla("").status == Status::error); }
    SUBCASE("not JSON") {
        const auto r = parse_ookla("Speedtest by Ookla\n   Download: 50 Mbps\n");
        CHECK(r.status == Status::error);
        CHECK(r.error == "no result in the speedtest output");
    }
    SUBCASE("truncated JSON") { CHECK(parse_ookla(R"({"type":"result","download":{)").status == Status::error); }
    SUBCASE("other types only") {
        CHECK(parse_ookla(R"({"type":"testStart","isp":"Example ISP"})").status == Status::error);
    }
    SUBCASE("wrong value types") {
        const auto r = parse_ookla(
            R"({"type":"result","download":{"bandwidth":"fast"},"upload":{"bandwidth":-1},"server":{"id":"12345"}})");
        CHECK(r.status == Status::error);
        CHECK_FALSE(r.download_mbps.has_value());
        CHECK_FALSE(r.upload_mbps.has_value());
        CHECK_FALSE(r.server_id.has_value());
    }
    SUBCASE("bandwidth 0 is missing, never a 0 Mbit/s measurement") {
        const auto r = parse_ookla(R"({"type":"result","download":{"bandwidth":1000000},"upload":{"bandwidth":0}})");
        CHECK(r.status == Status::error);
        CHECK_FALSE(r.upload_mbps.has_value());
    }
    SUBCASE("latency 0 is missing, and so is the jitter of that phase (#33)") {
        const auto r = parse_ookla(R"({"type":"result","download":{"bandwidth":1},)"
                                   R"("upload":{"bandwidth":1},"ping":{"latency":0,"jitter":0}})");
        CHECK(r.status == Status::ok);
        CHECK_FALSE(r.latency_ms.has_value());
        CHECK_FALSE(r.jitter_ms.has_value());
    }
    SUBCASE("an invalid or missing jitter leaves the latency") {
        for (const char* ping : {R"({"latency":8.5,"jitter":-1})", R"({"latency":8.5,"jitter":2e6})",
                                 R"({"latency":8.5})"}) {
            CAPTURE(ping);
            const auto r = parse_ookla(std::string{R"({"type":"result","download":{"bandwidth":1},)"
                                                   R"("upload":{"bandwidth":1},"ping":)"} +
                                       ping + "}");
            CHECK(r.latency_ms == doctest::Approx(8.5));
            CHECK_FALSE(r.jitter_ms.has_value());
        }
    }
    SUBCASE("a jitter of 0 next to a real latency is a real result") {
        const auto r = parse_ookla(R"({"type":"result","download":{"bandwidth":1},)"
                                   R"("upload":{"bandwidth":1},"ping":{"latency":8.5,"jitter":0}})");
        CHECK(r.latency_ms == doctest::Approx(8.5));
        REQUIRE(r.jitter_ms.has_value());
        CHECK(*r.jitter_ms == 0.0);
    }
    SUBCASE("out-of-range values stay empty") {
        const auto r = parse_ookla(
            R"({"type":"result","download":{"bandwidth":1},"upload":{"bandwidth":1},)"
            R"("ping":{"latency":-3,"jitter":-1},"packetLoss":150,"server":{"id":18446744073709551615}})");
        CHECK(r.status == Status::ok);
        CHECK_FALSE(r.latency_ms.has_value());
        CHECK_FALSE(r.jitter_ms.has_value());
        CHECK_FALSE(r.packet_loss_percent.has_value());
        CHECK_FALSE(r.server_id.has_value());
        CHECK_FALSE(parse_ookla(R"({"type":"result","server":{"id":-5}})").server_id.has_value());
    }
    SUBCASE("download without upload: values kept, but not ok") {
        const auto r = parse_ookla(R"({"type":"result","download":{"bandwidth":1000000}})");
        CHECK(r.status == Status::error);
        CHECK(r.download_mbps.has_value());
        CHECK_FALSE(r.raw.empty());
    }
}

TEST_CASE("parse_ookla: picks the result line among others") {
    const auto r = parse_ookla("{\"type\":\"testStart\"}\nnot json\n" + fixture("speedtest/ookla_ok.json"));
    CHECK(r.status == Status::ok);
    CHECK(r.server_id == 12345);
}

TEST_CASE("ookla_errors: both stderr formats, licence text ignored") {
    CHECK(ookla_errors(fixture("speedtest/ookla_no_servers.err")) ==
          "Configuration - No servers defined (NoServersException)");
    CHECK(ookla_errors(fixture("speedtest/ookla_bad_option.err")) ==
          "Invalid output format specified: bogus; Valid output formats: legacy, human-readable, "
          "csv, tsv, json, jsonl, json-pretty.");
    CHECK(ookla_errors("==============================================================================\n"
                       "You may only use this Speedtest software and information generated\n"
                       "{\"type\":\"log\",\"level\":\"warning\",\"message\":\"slow\"}\n")
              .empty());
}

TEST_CASE("ookla_errors: repeated lines appear once") {
    CHECK(ookla_errors(fixture("speedtest/ookla_offline.err")) ==
          "Trying to get interface information on non-initialized socket.; "
          "Configuration - Couldn't resolve host name (HostNotFoundException); "
          "Configuration - Cannot retrieve configuration document (0); "
          "ConfigurationError - Could not retrieve or read configuration (Configuration); "
          "Configuration - Could not retrieve or read configuration (ConfigurationError)");
}

TEST_CASE("OoklaBackend: command line") {
    FakeRunner fake{FakeRunner::exited(fixture("speedtest/ookla_ok.json"))};
    SUBCASE("default") {
        OoklaBackend b{{}, fake.runner()};
        CHECK(b.name() == "ookla");
        CHECK(b.measure().status == Status::ok);
        const auto& c = fake.calls.at(0);
        CHECK(c.program == "speedtest");
        CHECK(c.args == std::vector<std::string>{"--format=json", "--accept-license", "--accept-gdpr"});
        CHECK(c.timeout == 120s);
        REQUIRE(c.env.size() == 1);
        CHECK(c.env[0].first == "LC_ALL");
        CHECK(c.env[0].second == "C");
    }
    SUBCASE("pinned server, custom program and timeout") {
        OoklaBackend b{{.program = "/opt/ookla/speedtest", .server_id = 12345, .timeout = 90s}, fake.runner()};
        (void)b.measure();
        const auto& c = fake.calls.at(0);
        CHECK(c.program == "/opt/ookla/speedtest");
        CHECK(c.args.back() == "--server-id=12345");
        CHECK(c.timeout == 90s);
    }
    SUBCASE("invalid server id: nothing is run") {
        OoklaBackend b{{.server_id = 0}, fake.runner()};
        const auto r = b.measure();
        CHECK(r.status == Status::error);
        CHECK(fake.calls.empty());
    }
}

TEST_CASE("OoklaBackend: failures map to a status, never throw") {
    SUBCASE("not installed") {
        FakeRunner fake{FakeRunner::not_found()};
        const auto r = OoklaBackend{{}, fake.runner()}.measure();
        CHECK(r.status == Status::not_available);
        CHECK(r.backend == "ookla");
    }
    SUBCASE("timeout") {
        FakeRunner fake{FakeRunner::timed_out()};
        const auto r = OoklaBackend{{.server_id = std::nullopt, .timeout = 60s}, fake.runner()}.measure();
        CHECK(r.status == Status::timeout);
        CHECK(r.error == "speedtest gave no result within 60 s");
    }
    SUBCASE("timeout and signal keep the CLI's messages and raw output") {
        auto hung = FakeRunner::timed_out();
        hung.err = "[2026-01-15 12:00:00.000] [error] Timeout occurred in connect.\n";
        FakeRunner fake{hung};
        const auto r = OoklaBackend{{.server_id = std::nullopt, .timeout = 60s}, fake.runner()}.measure();
        CHECK(r.error == "speedtest gave no result within 60 s (Timeout occurred in connect.)");
        CHECK(r.raw.find("Timeout occurred") != std::string::npos);

        auto killed = FakeRunner::exited("", 0, hung.err);
        killed.outcome = meerkat::sys::ProcessResult::Outcome::signaled;
        killed.signal = 9;
        FakeRunner fake2{killed};
        const auto k = OoklaBackend{{}, fake2.runner()}.measure();
        CHECK(k.status == Status::error);
        CHECK(k.error == "speedtest was killed by signal 9 (Timeout occurred in connect.)");
        CHECK_FALSE(k.raw.empty());
    }
    SUBCASE("offline (real output in an empty network namespace)") {
        FakeRunner fake{FakeRunner::exited("", 2, fixture("speedtest/ookla_offline.err"))};
        const auto r = OoklaBackend{{}, fake.runner()}.measure();
        CHECK(r.status == Status::no_network);
        CHECK(r.error.find("HostNotFoundException") != std::string::npos);
    }
    SUBCASE("CLI error: Ookla's message, stderr kept as raw") {
        FakeRunner fake{FakeRunner::exited("", 2, fixture("speedtest/ookla_no_servers.err"))};
        const auto r = OoklaBackend{{.server_id = 1}, fake.runner()}.measure();
        CHECK(r.status == Status::error);
        CHECK(r.error == "Configuration - No servers defined (NoServersException)");
        CHECK(r.raw.find("NoServersException") != std::string::npos);
        CHECK_FALSE(r.download_mbps.has_value());
    }
    SUBCASE("Python speedtest-cli under the same name") {
        FakeRunner fake{FakeRunner::exited(
            "", 2, "usage: speedtest [-h] [--no-download] ...\nspeedtest: error: unrecognized arguments: "
                   "--format=json --accept-license --accept-gdpr\n")};
        const auto r = OoklaBackend{{}, fake.runner()}.measure();
        CHECK(r.status == Status::error);
        CHECK(r.error.find("is not the Ookla CLI") != std::string::npos);
    }
    SUBCASE("exit 0 without a result") {
        FakeRunner fake{FakeRunner::exited("", 0)};
        const auto r = OoklaBackend{{}, fake.runner()}.measure();
        CHECK(r.status == Status::error);
        CHECK(r.error == "no result in the speedtest output");
    }
    SUBCASE("result despite a non-zero exit: kept, with the error") {
        FakeRunner fake{FakeRunner::exited(fixture("speedtest/ookla_ok.json"), 1,
                                           "[2026-01-15 12:00:00.000] [error] Cannot save result\n")};
        const auto r = OoklaBackend{{}, fake.runner()}.measure();
        CHECK(r.status == Status::ok);
        CHECK(r.error == "Cannot save result");
    }
    SUBCASE("result with exit 0 and an error line: still ok, message kept") {
        FakeRunner fake{FakeRunner::exited(fixture("speedtest/ookla_ok.json"), 0,
                                           "[2026-01-15 12:00:00.000] [error] Cannot save result\n")};
        const auto r = OoklaBackend{{}, fake.runner()}.measure();
        CHECK(r.status == Status::ok);
        CHECK(r.error == "Cannot save result");
    }
    SUBCASE("runner throws") {
        const auto r = OoklaBackend{{}, [](const meerkat::sys::Command&) -> meerkat::sys::ProcessResult {
                           throw std::system_error(std::make_error_code(std::errc::too_many_files_open));
                       }}.measure();
        CHECK(r.status == Status::error);
        CHECK(r.error.starts_with("cannot run speedtest: "));
        const auto s = OoklaBackend{{}, [](const meerkat::sys::Command&) -> meerkat::sys::ProcessResult {
                           throw 42;
                       }}.measure();
        CHECK(s.error == "cannot run speedtest: unknown exception");
    }
}

TEST_CASE("OoklaBackend through the SpeedBackend interface") {
    FakeRunner fake{FakeRunner::exited(fixture("speedtest/ookla_ok.json"))};
    std::unique_ptr<SpeedBackend> backend = std::make_unique<OoklaBackend>(OoklaOptions{}, fake.runner());
    const auto r = backend->measure();
    CHECK(r.status == Status::ok);
    CHECK(r.backend == backend->name());
    CHECK(parse_ookla("").backend == backend->name());  // one spelling (#33)
}
