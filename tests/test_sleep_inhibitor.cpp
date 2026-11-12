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
        CHECK(seen.what == "sleep:idle");
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

TEST_CASE("SleepInhibitor: failures carry a kind and a hint") {
    SUBCASE("polkit refuses") {
        InhibitReply r;
        r.error = EACCES;
        r.error_name = "org.freedesktop.DBus.Error.AccessDenied";
        r.error_message = "Permission denied";
        const auto e = error_for(r);
        CHECK(e.kind() == InhibitError::Kind::denied);
        CHECK(has(e.what(), "can't block suspend: Permission denied "
                            "(org.freedesktop.DBus.Error.AccessDenied)."));
        CHECK(has(e.hint(), "polkit refused org.freedesktop.login1.inhibit-block-sleep, "
                            "org.freedesktop.login1.inhibit-block-idle."));
        CHECK(has(e.hint(), "systemctl mask sleep.target"));
        CHECK(has(e.what(), e.hint()));
    }
    SUBCASE("interactive authorization would be needed") {
        InhibitReply r;
        r.error = EACCES;
        r.error_name = "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired";
        const auto e = error_for(r, "sleep:handle-lid-switch");
        CHECK(e.kind() == InhibitError::Kind::denied);
        // handle-* actions have no mode in their name.
        CHECK(has(e.hint(), "org.freedesktop.login1.inhibit-block-sleep, "
                            "org.freedesktop.login1.inhibit-handle-lid-switch."));
        // No message from D-Bus: the errno text stands in.
        CHECK(has(e.what(), "Permission denied"));
    }
    SUBCASE("no system bus") {
        InhibitReply r;
        r.error = ENOENT;
        r.no_bus = true;
        r.error_message = "can't connect to the system D-Bus: No such file or directory";
        const auto e = error_for(r);
        CHECK(e.kind() == InhibitError::Kind::no_bus);
        CHECK(has(e.hint(), "No system D-Bus"));
    }
    SUBCASE("logind isn't running") {
        InhibitReply r;
        r.error = EHOSTUNREACH;
        r.error_name = "org.freedesktop.DBus.Error.ServiceUnknown";
        r.error_message = "The name org.freedesktop.login1 was not provided by any .service files";
        const auto e = error_for(r);
        CHECK(e.kind() == InhibitError::Kind::no_logind);
        CHECK(has(e.hint(), "systemd-logind isn't running"));
    }
    SUBCASE("anything else") {
        InhibitReply r;
        r.error = ETIMEDOUT;
        r.error_name = "org.freedesktop.DBus.Error.Timeout";
        r.error_message = "Connection timed out";
        const auto e = error_for(r);
        CHECK(e.kind() == InhibitError::Kind::failed);
        CHECK(has(e.what(), "Connection timed out (org.freedesktop.DBus.Error.Timeout)"));
        CHECK(has(e.hint(), "systemctl mask"));
    }
    SUBCASE("no fd and no error at all") {
        const auto e = error_for(InhibitReply{});
        CHECK(e.kind() == InhibitError::Kind::failed);
        CHECK(has(e.what(), "logind returned no lock"));
    }
}

TEST_CASE("logind_inhibit: the real call returns a lock or an error, never both") {
    // Works with or without a system bus (CI containers have none): only the
    // shape of the reply is checked. A lock taken here is released at once.
    const auto r = logind_inhibit({"sleep", "meerkat tests", "unit test", "delay"});
    if (r.fd >= 0) {
        CHECK(r.error == 0);
        CHECK(r.fd > 2);
        CHECK((::fcntl(r.fd, F_GETFD) & FD_CLOEXEC) != 0);
        ::close(r.fd);
    } else {
        CHECK(r.error > 0);
        CHECK_FALSE(r.error_message.empty());
        MESSAGE("no logind lock here: " << r.error_message);
    }
}
