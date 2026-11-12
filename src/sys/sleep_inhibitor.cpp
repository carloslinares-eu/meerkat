/**
 * @file sleep_inhibitor.cpp
 * @brief Implementation of the logind sleep inhibitor.
 */
#include "sys/sleep_inhibitor.hpp"

#include <fcntl.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

namespace meerkat::sys {

namespace {

/// How long to wait for logind's answer.
constexpr std::uint64_t kCallTimeoutUsec = 10'000'000;

/// Closes and frees a bus connection.
struct BusCloser {
    /// @param b The bus.
    void operator()(sd_bus* b) const noexcept { sd_bus_flush_close_unref(b); }
};

/// Frees a message.
struct MessageUnref {
    /// @param m The message.
    void operator()(sd_bus_message* m) const noexcept { sd_bus_message_unref(m); }
};

/**
 * @brief `strerror` for a positive errno value, thread-safe.
 * @param err The errno value.
 * @return Its text.
 */
std::string errno_text(int err) {
    char buf[256] = {};
    // GNU strerror_r returns the text, which may or may not be in buf.
    return ::strerror_r(err, buf, sizeof buf);
}

/**
 * @brief The polkit actions a request needs, e.g.
 *        `org.freedesktop.login1.inhibit-block-sleep`.
 * @param r The request.
 * @return The action names, comma-separated.
 */
std::string polkit_actions(const InhibitRequest& r) {
    std::string out;
    std::string_view what = r.what;
    while (!what.empty()) {
        const auto colon = what.find(':');
        const auto item = what.substr(0, colon);
        // logind has delay locks only for sleep and shutdown; it rejects the
        // others with InvalidArgs before polkit is asked.
        const bool exists = r.mode != "delay" || item == "sleep" || item == "shutdown" ||
                            item.starts_with("handle-");
        if (!item.empty() && exists) {
            if (!out.empty()) {
                out += ", ";
            }
            out += "org.freedesktop.login1.inhibit-";
            // The handle-* locks have one action each, whatever the mode.
            if (!item.starts_with("handle-")) {
                out += r.mode + "-";
            }
            out += item;
        }
        what = colon == std::string_view::npos ? std::string_view{} : what.substr(colon + 1);
    }
    return out;
}

/// Hint shared by every failure: how to keep the computer awake without meerkat.
constexpr std::string_view kManualFallback =
    "Until then, turn off automatic suspend in the system settings, or stop "
    "suspend altogether with `sudo systemctl mask sleep.target suspend.target "
    "hibernate.target hybrid-sleep.target` (undo with `unmask`).";

}  // namespace

InhibitError::Kind classify_dbus_error(std::string_view error_name) {
    using Kind = InhibitError::Kind;
    if (error_name == "org.freedesktop.DBus.Error.AccessDenied" ||
        error_name == "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired" ||
        error_name == "org.freedesktop.DBus.Error.AuthFailed") {
        return Kind::denied;
    }
    if (error_name == "org.freedesktop.DBus.Error.ServiceUnknown" ||
        error_name == "org.freedesktop.DBus.Error.NameHasNoOwner") {
        return Kind::no_logind;
    }
    return Kind::failed;
}

InhibitReply logind_inhibit(const InhibitRequest& request) {
    InhibitReply reply;
    sd_bus* raw_bus = nullptr;
    int r = sd_bus_open_system(&raw_bus);
    std::unique_ptr<sd_bus, BusCloser> bus{raw_bus};
    if (r < 0) {
        reply.failure = InhibitError::Kind::no_bus;
        reply.message = "can't connect to the system D-Bus: " + errno_text(-r);
        return reply;
    }
    sd_bus_set_method_call_timeout(bus.get(), kCallTimeoutUsec);

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* raw_msg = nullptr;
    r = sd_bus_call_method(bus.get(), "org.freedesktop.login1", "/org/freedesktop/login1",
                           "org.freedesktop.login1.Manager", "Inhibit", &error, &raw_msg, "ssss",
                           request.what.c_str(), request.who.c_str(), request.why.c_str(),
                           request.mode.c_str());
    std::unique_ptr<sd_bus_message, MessageUnref> msg{raw_msg};
    if (r < 0) {
        reply.error_name = error.name != nullptr ? error.name : "";
        reply.failure = classify_dbus_error(reply.error_name);
        reply.message = error.message != nullptr ? error.message : errno_text(-r);
        sd_bus_error_free(&error);
        return reply;
    }
    sd_bus_error_free(&error);

    int fd = -1;
    r = sd_bus_message_read(msg.get(), "h", &fd);
    if (r <= 0 || fd < 0) {
        reply.message = "logind's reply has no lock" + (r < 0 ? ": " + errno_text(-r) : "");
        return reply;
    }
    // The fd belongs to the message, which is freed on return: keep a copy,
    // above stdio and closed on exec so programs we start don't hold the lock.
    reply.fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (reply.fd < 0) {
        reply.message = "can't keep the lock: " + errno_text(errno);
    }
    return reply;
}

InhibitError inhibit_error(const InhibitRequest& request, const InhibitReply& reply) {
    using Kind = InhibitError::Kind;
    const Kind kind = reply.failure;
    std::string detail = reply.message.empty() ? "logind returned no lock" : reply.message;
    if (!reply.error_name.empty()) {
        detail += " (" + reply.error_name + ")";
    }

    std::string hint;
    switch (kind) {
    case Kind::no_bus:
        hint = "No system D-Bus here (a container, or no systemd?), so meerkat can't keep "
               "the computer awake. ";
        break;
    case Kind::no_logind:
        hint = "systemd-logind isn't running, so meerkat can't keep the computer awake. ";
        break;
    case Kind::denied:
        hint = "Access was denied, usually by polkit (or else by a D-Bus, SELinux or "
               "AppArmor policy). polkit needs " + polkit_actions(request) +
               "; by default only a user logged in at the computer (an active local "
               "session) gets them, not an SSH login or a system service. Allow them for "
               "meerkat's user with a polkit rule (see `man polkit`). ";
        break;
    case Kind::failed:
        hint = "meerkat can't keep the computer awake. ";
        break;
    }
    hint += kManualFallback;
    return InhibitError{kind, "can't block suspend: " + detail + ". " + hint, hint};
}

SleepInhibitor SleepInhibitor::acquire(const InhibitRequest& request, const InhibitCall& call) {
    InhibitReply reply = call(request);
    if (reply.fd < 0) {
        throw inhibit_error(request, reply);
    }
    return SleepInhibitor{reply.fd};
}

SleepInhibitor& SleepInhibitor::operator=(SleepInhibitor&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void SleepInhibitor::release() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace meerkat::sys
