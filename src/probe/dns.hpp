/**
 * @file dns.hpp
 * @brief Times a DNS lookup through the system resolver.
 */
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "core/measurement.hpp"

namespace meerkat::probe {

/**
 * @brief Does one blocking lookup and returns a getaddrinfo() error code
 *        (0 on success). The default calls getaddrinfo() itself; tests
 *        pass a fake. For `EAI_SYSTEM` it leaves the cause in `errno`, which
 *        resolve() reads right after the call on the same thread.
 * @warning resolve() copies the resolver into a thread that may outlive the
 *          call (when the lookup hangs). A resolver must therefore own what
 *          it uses: no captures by reference and no `this`.
 */
using Resolver = std::function<int(const std::string& host)>;

/// How long resolve() waits by default.
inline constexpr std::chrono::milliseconds kDnsTimeout{5000};

/**
 * @brief Resolves @p host with getaddrinfo() (AF_UNSPEC).
 *
 * AI_ADDRCONFIG is deliberately not used: without a non-loopback address it
 * makes every lookup fail as "name not known", which would hide an outage
 * behind a misleading reason.
 * @param host Host name.
 * @return getaddrinfo()'s return value: 0 or an `EAI_*` code.
 */
int system_resolve(const std::string& host);

/**
 * @brief Checks that @p host looks like a DNS name or address literal.
 *
 * 1–253 characters, no whitespace or control characters
 * (core::text::is_host_text()).
 *
 * @param host The name.
 * @return True if it can be looked up.
 */
bool is_valid_dns_host(std::string_view host);

/**
 * @brief Looks up @p host and measures how long the resolver takes.
 *
 * getaddrinfo() has no timeout of its own, so the lookup runs on a helper
 * thread and the caller waits at most @p timeout. A lookup that is still
 * hanging then is left to finish in the background (with its own copies of
 * the name and the resolver) and the result is `timeout`. At most 8 lookups
 * may hang at once; beyond that, resolve() returns `error` without starting
 * another, so a dead DNS server can't pile up threads.
 *
 * `duration_ms` is the time of the resolver call itself, measured on the
 * helper thread (thread start-up isn't counted).
 *
 * The time includes whatever the system resolver does: /etc/hosts, a local
 * cache such as systemd-resolved, then the DNS servers. A cached answer is
 * fast; that is still what applications on this computer experience.
 *
 * Status mapping:
 * - `ok`: success, with `duration_ms`.
 * - `no_network`: the resolver's system call found no local route
 *   (`EAI_SYSTEM` with `ENETUNREACH` or `ENETDOWN`).
 * - `timeout`: no answer within @p timeout.
 * - `error`: anything else, with the resolver's message (for `EAI_SYSTEM`
 *   the errno text is added). That includes `EAI_AGAIN` ("Temporary failure
 *   in name resolution": no DNS server answered), which a router answering
 *   SERVFAIL causes as well as an offline computer; callers that know the
 *   link use core::LinkSnapshot::probe_status() to tell the two apart.
 *
 * @param host     Name to resolve; see is_valid_dns_host().
 * @param timeout  Longest wait (negative counts as 0).
 * @param resolver How to resolve (tests pass a fake).
 * @return The result; `host` is always filled in. Never throws.
 */
core::DnsResult resolve(std::string_view host,
                        std::chrono::milliseconds timeout = kDnsTimeout,
                        const Resolver& resolver = system_resolve);

}  // namespace meerkat::probe
