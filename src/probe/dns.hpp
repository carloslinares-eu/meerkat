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
 *        pass a fake.
 * @warning resolve() copies the resolver into a thread that may outlive the
 *          call (when the lookup hangs). A resolver must therefore own what
 *          it uses: no captures by reference and no `this`.
 */
using Resolver = std::function<int(const std::string& host)>;

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
 * 1–253 characters, no whitespace or control characters.
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
 * Status mapping: success → `ok` with `duration_ms`; no answer within
 * @p timeout → `timeout`; anything else (`EAI_NONAME`: the name doesn't
 * exist, `EAI_AGAIN`: no DNS server answered, …) → `error` with the
 * resolver's message.
 *
 * @param host     Name to resolve; see is_valid_dns_host().
 * @param timeout  Longest wait (negative counts as 0).
 * @param resolver How to resolve (tests pass a fake).
 * @return The result; `host` is always filled in. Never throws.
 */
core::DnsResult resolve(std::string_view host,
                        std::chrono::milliseconds timeout = std::chrono::seconds{5},
                        const Resolver& resolver = system_resolve);

}  // namespace meerkat::probe
