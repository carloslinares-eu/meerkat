/**
 * @file dns.cpp
 * @brief Implementation of the DNS probe.
 */
#include "probe/dns.hpp"

#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <future>
#include <thread>
#include <utility>

#include "core/text.hpp"

namespace meerkat::probe {

namespace {

/// Most lookups that may still be hanging in the background at once.
constexpr int kMaxPending = 8;

/// Number of lookup threads that haven't finished yet.
std::atomic<int> g_pending{0};

/// What the lookup thread hands back: getaddrinfo() code and its run time.
struct Outcome {
    int code = 0;          ///< 0 or an `EAI_*` code.
    int sys_errno = 0;     ///< errno of the helper thread, for `EAI_SYSTEM`.
    double duration_ms{};  ///< Time spent in the resolver.
};

/**
 * @brief Turns a failed lookup into a status and a message.
 * @param r       The result to fill in.
 * @param outcome What the resolver returned.
 */
void set_failure(core::DnsResult& r, const Outcome& outcome) {
    if (outcome.code == EAI_SYSTEM) {
        const int e = outcome.sys_errno;
        r.error = std::string{::gai_strerror(EAI_SYSTEM)} +
                  (e != 0 ? std::string{": "} + core::text::errno_text(e) : std::string{});
        // Only a missing local route. EHOSTUNREACH can come from a remote
        // router's ICMP reply, so it stays `error`.
        if (e == ENETUNREACH || e == ENETDOWN) {
            r.status = core::Status::no_network;
        }
        return;
    }
    // EAI_AGAIN ("no DNS server answered") stays `error`: a router that is
    // up but answers SERVFAIL gives it too. LinkSnapshot::probe_status()
    // turns it into `no_network` when the computer really had no connection.
    r.error = ::gai_strerror(outcome.code);
}

}  // namespace

int system_resolve(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (result != nullptr) {
        ::freeaddrinfo(result);
    }
    return rc;
}

core::DnsResult resolve(std::string_view host, std::chrono::milliseconds timeout,
                        const Resolver& resolver) {
    core::DnsResult r;
    try {
        r.host = std::string{host};
        if (!core::text::is_host_text(host)) {  // what validate() checks
            r.error = "invalid host name";
            return r;
        }
        timeout = std::max(timeout, std::chrono::milliseconds{0});

        if (g_pending.fetch_add(1) >= kMaxPending) {
            g_pending.fetch_sub(1);
            r.error = "too many earlier DNS lookups are still hanging";
            return r;
        }
        std::promise<Outcome> promise;
        auto future = promise.get_future();
        try {
            // The thread gets its own copies of the name and the resolver, so
            // it can outlive this call safely if the lookup hangs.
            std::thread([promise = std::move(promise), name = r.host, resolver]() mutable {
                try {
                    const auto start = std::chrono::steady_clock::now();
                    errno = 0;
                    const int code = resolver(name);
                    const int sys_errno = errno;  // errno is per thread: read it here
                    const std::chrono::duration<double, std::milli> took =
                        std::chrono::steady_clock::now() - start;
                    promise.set_value(
                        {.code = code, .sys_errno = sys_errno, .duration_ms = took.count()});
                } catch (...) {
                    promise.set_exception(std::current_exception());
                }
                g_pending.fetch_sub(1);
            }).detach();
        } catch (...) {
            g_pending.fetch_sub(1);
            throw;
        }

        if (future.wait_for(timeout) != std::future_status::ready) {
            r.status = core::Status::timeout;
            r.error = "no answer within " + std::to_string(timeout.count()) + " ms";
            return r;
        }
        Outcome outcome;
        try {
            outcome = future.get();
        } catch (const std::exception& e) {
            r.error = std::string{"resolver failed: "} + e.what();
            return r;
        } catch (...) {
            r.error = "resolver failed: unknown exception";
            return r;
        }
        if (outcome.code != 0) {
            set_failure(r, outcome);
            return r;
        }
        r.status = core::Status::ok;
        r.duration_ms = outcome.duration_ms;
    } catch (const std::exception& e) {
        r.status = core::Status::error;
        r.error = std::string{"cannot run the lookup: "} + e.what();
    } catch (...) {
        r.status = core::Status::error;
        r.error = "cannot run the lookup: unknown exception";
    }
    return r;
}

}  // namespace meerkat::probe
