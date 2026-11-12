/**
 * @file sleep_inhibitor.hpp
 * @brief Keeps the computer from suspending while meerkat records, with a
 *        logind inhibitor lock.
 */
#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace meerkat::sys {

/// What to ask logind for; see `org.freedesktop.login1.Manager.Inhibit`.
struct InhibitRequest {
    /// Colon-separated lock types. `sleep` blocks suspend and hibernate,
    /// `idle` the automatic idle action, and `handle-lid-switch` the lid:
    /// logind ignores `sleep` locks when the lid closes, unless
    /// `LidSwitchIgnoreInhibited=no` is set in logind.conf (default `yes`).
    std::string what = "sleep:idle:handle-lid-switch";
    std::string who = "meerkat";                                   ///< Shown by `systemd-inhibit --list`.
    std::string why = "Recording internet speed and connectivity"; ///< Shown by `systemd-inhibit --list`.
    std::string mode = "block";                                    ///< `block` or `delay`.
};

/// Why a lock couldn't be taken.
class InhibitError : public std::runtime_error {
public:
    /// The kind of failure, which decides the hint.
    enum class Kind {
        no_bus,     ///< The system D-Bus can't be reached (container, no systemd).
        no_logind,  ///< The bus is there but systemd-logind isn't.
        denied,     ///< Access denied: polkit, or a D-Bus/SELinux/AppArmor policy.
        failed,     ///< Anything else.
    };

    /**
     * @brief An error with its kind and a hint on what to do.
     * @param kind    The kind of failure.
     * @param message What happened, including the hint.
     * @param hint    What to do about it (also part of @p message).
     */
    InhibitError(Kind kind, const std::string& message, std::string hint)
        : std::runtime_error(message), kind_{kind}, hint_{std::move(hint)} {}

    /// @return The kind of failure.
    [[nodiscard]] Kind kind() const noexcept { return kind_; }

    /// @return What the user can do about it.
    [[nodiscard]] const std::string& hint() const noexcept { return hint_; }

private:
    Kind kind_;         ///< The kind of failure.
    std::string hint_;  ///< What to do.
};

/// What the D-Bus call returned: a lock fd, or why there is none.
struct InhibitReply {
    int fd = -1;  ///< The lock, owned by the receiver; -1 on failure.
    InhibitError::Kind failure = InhibitError::Kind::failed;  ///< Why, when `fd` is -1.
    std::string error_name;  ///< D-Bus error name, if any, e.g. `org.freedesktop.DBus.Error.AccessDenied`.
    std::string message;     ///< What went wrong, from D-Bus or the bus library; empty on success.
};

/// Makes the inhibit call. Replaceable in tests; logind_inhibit() is the real one.
using InhibitCall = std::function<InhibitReply(const InhibitRequest&)>;

/**
 * @brief Sorts a D-Bus error from the Inhibit call into a failure kind.
 * @param error_name The D-Bus error name.
 * @return InhibitError::Kind::denied, InhibitError::Kind::no_logind or
 *         InhibitError::Kind::failed.
 */
InhibitError::Kind classify_dbus_error(std::string_view error_name);

/**
 * @brief Calls logind's `Inhibit()` on the system bus through sd-bus.
 *
 * Never asks for interactive authorization: meerkat runs unattended, so
 * polkit either allows the lock outright or refuses it. Waits at most 10 s
 * for logind.
 * @param request What to inhibit.
 * @return The lock fd (close-on-exec, owned by the caller), or the error.
 */
InhibitReply logind_inhibit(const InhibitRequest& request);

/**
 * @brief Turns a failed reply into an InhibitError with a hint.
 * @param request The request that failed (its `what` goes into the hint).
 * @param reply   The failed reply (`fd` is -1).
 * @return The error.
 */
InhibitError inhibit_error(const InhibitRequest& request, const InhibitReply& reply);

/**
 * @brief Holds a logind inhibitor lock for as long as it lives.
 *
 * The lock is a file descriptor: logind drops it as soon as the descriptor
 * is closed, which also happens when meerkat crashes or is killed, so a
 * dead meerkat never keeps the computer awake.
 */
class SleepInhibitor {
public:
    /// No lock.
    SleepInhibitor() = default;

    /**
     * @brief Takes the lock.
     * @param request What to inhibit.
     * @param call    How to call logind (logind_inhibit() unless testing).
     * @return The held lock.
     * @throws InhibitError if logind can't be reached or refuses; the
     *         message says why and what to do.
     */
    static SleepInhibitor acquire(const InhibitRequest& request = {},
                                  const InhibitCall& call = logind_inhibit);

    /// Releases the lock.
    ~SleepInhibitor() { release(); }

    SleepInhibitor(const SleepInhibitor&) = delete;             ///< Owns a lock fd: not copyable.
    SleepInhibitor& operator=(const SleepInhibitor&) = delete;  ///< Owns a lock fd: not copyable.

    /**
     * @brief Takes over @p other's lock.
     * @param other The lock to take over; holds nothing afterwards.
     */
    SleepInhibitor(SleepInhibitor&& other) noexcept : fd_{other.fd_} { other.fd_ = -1; }

    /**
     * @brief Releases this lock and takes over @p other's.
     * @param other The lock to take over; holds nothing afterwards.
     * @return This object.
     */
    SleepInhibitor& operator=(SleepInhibitor&& other) noexcept;

    /// @return True while a lock is held.
    [[nodiscard]] bool held() const noexcept { return fd_ >= 0; }

    /// Releases the lock now; does nothing if none is held.
    void release() noexcept;

private:
    /**
     * @brief Wraps a lock fd.
     * @param fd The lock, now owned.
     */
    explicit SleepInhibitor(int fd) noexcept : fd_{fd} {}

    int fd_ = -1;  ///< The lock, or -1.
};

}  // namespace meerkat::sys
