/**
 * @file test_sleep_inhibitor.cpp
 * @brief Tests for sys::SleepInhibitor with a fake logind call.
 */
#include <doctest/doctest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <utility>

#include "sys/sleep_inhibitor.hpp"

using namespace meerkat::sys;

namespace {

/// A pipe; the inhibitor gets the write end as its "lock". Reading the read
/// end returns EOF once every copy of the write end is closed.
struct LockPipe {
    int read_end = -1;   ///< Kept by the test.
    int write_end = -1;  ///< Handed out as the lock fd.

    LockPipe() {
        int fds[2];
        REQUIRE(::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0);
        read_end = fds[0];
        write_end = fds[1];
    }
    ~LockPipe() { ::close(read_end); }
    LockPipe(const LockPipe&) = delete;
    LockPipe& operator=(const LockPipe&) = delete;

    /// True while some write end is still open (the lock is held).
    [[nodiscard]] bool held() const {
        char c = 0;
        const auto n = ::read(read_end, &c, 1);
        return n < 0 && errno == EAGAIN;  // 0 = EOF: released
    }

    /// A call that grants the lock and records the request.
    InhibitCall grant(InhibitRequest& seen) {
        return [this, &seen](const InhibitRequest& r) {
            seen = r;
            InhibitReply reply;
            reply.fd = write_end;
            return reply;
        };
    }
};

/// A call that fails with @p reply.
InhibitCall refuse(InhibitReply reply) {
    return [reply = std::move(reply)](const InhibitRequest&) { return reply; };
}

/// The error acquire() throws for @p reply (with @p what as the request).
InhibitError error_for(InhibitReply reply, std::string what = "sleep:idle") {
    InhibitRequest req;
    req.what = std::move(what);
    try {
        (void)SleepInhibitor::acquire(req, refuse(std::move(reply)));
    } catch (const InhibitError& e) {
        return e;
    }
    FAIL("acquire() didn't throw");
    return InhibitError{InhibitError::Kind::failed, "", ""};
}

/// True if @p text contains @p part.
bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("SleepInhibitor holds the lock fd until it is released") {
    LockPipe pipe;
    InhibitRequest seen;

    SUBCASE("the request reaches logind as given") {
        InhibitRequest req;
        req.what = "sleep:idle:handle-lid-switch";
        req.who = "test";
        req.why = "testing";
        auto lock = SleepInhibitor::acquire(req, pipe.grant(seen));
        CHECK(seen.what == "sleep:idle:handle-lid-switch");
        CHECK(seen.who == "test");
        CHECK(seen.why == "testing");
        CHECK(seen.mode == "block");
    }
    SUBCASE("the defaults") {
        auto lock = SleepInhibitor::acquire({}, pipe.grant(seen));
        CHECK(seen.what == "sleep:idle:handle-lid-switch");
        CHECK(seen.who == "meerkat");
        CHECK(seen.mode == "block");
    }
    SUBCASE("destruction releases") {
        {
            auto lock = SleepInhibitor::acquire({}, pipe.grant(seen));
            CHECK(lock.held());
            CHECK(pipe.held());
        }
        CHECK_FALSE(pipe.held());
    }
    SUBCASE("release() releases, and again does nothing") {
        auto lock = SleepInhibitor::acquire({}, pipe.grant(seen));
        lock.release();
        CHECK_FALSE(lock.held());
        CHECK_FALSE(pipe.held());
        lock.release();
        CHECK_FALSE(lock.held());
    }
    SUBCASE("moving hands the lock over") {
        auto a = SleepInhibitor::acquire({}, pipe.grant(seen));
        SleepInhibitor b{std::move(a)};
        CHECK_FALSE(a.held());  // NOLINT(bugprone-use-after-move): moved-from state is specified
        CHECK(b.held());
        SleepInhibitor c;
        CHECK_FALSE(c.held());
        c = std::move(b);
        CHECK(c.held());
        CHECK(pipe.held());
        c = SleepInhibitor{};
        CHECK_FALSE(pipe.held());
    }
    SUBCASE("move-assigning releases the old lock") {
        LockPipe other;
        InhibitRequest seen2;
        auto a = SleepInhibitor::acquire({}, pipe.grant(seen));
        a = SleepInhibitor::acquire({}, other.grant(seen2));
        CHECK_FALSE(pipe.held());
        CHECK(other.held());
    }
}

TEST_CASE("classify_dbus_error") {
    using Kind = InhibitError::Kind;
    CHECK(classify_dbus_error("org.freedesktop.DBus.Error.AccessDenied") == Kind::denied);
    CHECK(classify_dbus_error("org.freedesktop.DBus.Error.InteractiveAuthorizationRequired") ==
          Kind::denied);
    CHECK(classify_dbus_error("org.freedesktop.DBus.Error.AuthFailed") == Kind::denied);
    CHECK(classify_dbus_error("org.freedesktop.DBus.Error.ServiceUnknown") == Kind::no_logind);
    CHECK(classify_dbus_error("org.freedesktop.DBus.Error.NameHasNoOwner") == Kind::no_logind);
    CHECK(classify_dbus_error("org.freedesktop.DBus.Error.Timeout") == Kind::failed);
    CHECK(classify_dbus_error("") == Kind::failed);
}

TEST_CASE("SleepInhibitor: failures carry a kind and a hint") {
    using Kind = InhibitError::Kind;
    SUBCASE("access denied: the polkit actions for every lock type") {
        const auto e = error_for({-1, Kind::denied, "org.freedesktop.DBus.Error.AccessDenied",
                                  "Permission denied"},
                                 "sleep:idle:handle-lid-switch");
        CHECK(e.kind() == Kind::denied);
        CHECK(has(e.what(), "can't block suspend: Permission denied "
                            "(org.freedesktop.DBus.Error.AccessDenied)."));
        // handle-* actions have no mode in their name.
        CHECK(has(e.hint(), "polkit needs org.freedesktop.login1.inhibit-block-sleep, "
                            "org.freedesktop.login1.inhibit-block-idle, "
                            "org.freedesktop.login1.inhibit-handle-lid-switch;"));
        CHECK(has(e.hint(), "SELinux"));
        CHECK(has(e.hint(), "systemctl mask sleep.target"));
        CHECK(has(e.what(), e.hint()));
    }
    SUBCASE("delay mode: only the delay actions that exist") {
        InhibitRequest req;
        req.what = "sleep:idle";
        req.mode = "delay";
        const auto e = inhibit_error(req, {-1, Kind::denied, "", "denied"});
        CHECK(has(e.hint(), "polkit needs org.freedesktop.login1.inhibit-delay-sleep;"));
        CHECK_FALSE(has(e.hint(), "delay-idle"));
    }
    SUBCASE("no system bus") {
        const auto e =
            error_for({-1, Kind::no_bus, "", "can't connect to the system D-Bus: No such file"});
        CHECK(e.kind() == Kind::no_bus);
        CHECK(has(e.what(), "can't connect to the system D-Bus: No such file."));
        CHECK(has(e.hint(), "No system D-Bus"));
    }
    SUBCASE("logind isn't running") {
        const auto e = error_for({-1, Kind::no_logind, "org.freedesktop.DBus.Error.ServiceUnknown",
                                  "The name org.freedesktop.login1 was not provided"});
        CHECK(e.kind() == Kind::no_logind);
        CHECK(has(e.hint(), "systemd-logind isn't running"));
    }
    SUBCASE("anything else") {
        const auto e = error_for(
            {-1, Kind::failed, "org.freedesktop.DBus.Error.Timeout", "Connection timed out"});
        CHECK(e.kind() == Kind::failed);
        CHECK(has(e.what(), "Connection timed out (org.freedesktop.DBus.Error.Timeout)"));
        CHECK(has(e.hint(), "systemctl mask"));
    }
    SUBCASE("no fd and no message at all") {
        const auto e = error_for(InhibitReply{});
        CHECK(e.kind() == Kind::failed);
        CHECK(has(e.what(), "logind returned no lock"));
    }
}

TEST_CASE("logind_inhibit: the real call returns a lock or an error, never both") {
    // Works with or without a system bus (CI containers have none): only the
    // shape of the reply is checked. A lock taken here is released at once.
    const auto r = logind_inhibit({"sleep", "meerkat tests", "unit test", "delay"});
    if (r.fd >= 0) {
        CHECK(r.message.empty());
        CHECK(r.fd > 2);
        CHECK((::fcntl(r.fd, F_GETFD) & FD_CLOEXEC) != 0);
        ::close(r.fd);
    } else {
        CHECK_FALSE(r.message.empty());
        MESSAGE("no logind lock here: " << r.message);
    }
}
