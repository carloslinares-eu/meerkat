/**
 * @file dns.cpp
 * @brief Implementation of the DNS probe.
 */
#include "probe/dns.hpp"

#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <future>
#include <thread>
#include <utility>

namespace meerkat::probe {

namespace {

/// Most lookups that may still be hanging in the background at once.
constexpr int kMaxPending = 8;

/// Number of lookup threads that haven't finished yet.
std::atomic<int> g_pending{0};

/// What the lookup thread hands back: getaddrinfo() code and its run time.
struct Outcome {
    int code = 0;          ///< 0 or an `EAI_*` code.
    double duration_ms{};  ///< Time spent in the resolver.
};

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

bool is_valid_dns_host(std::string_view host) {
    return !host.empty() && host.size() <= 253 &&
           std::ranges::none_of(host, [](char c) {
               return static_cast<unsigned char>(c) <= ' ' || c == '\x7f';
           });
}

core::DnsResult resolve(std::string_view host, std::chrono::milliseconds timeout,
                        const Resolver& resolver) {
    core::DnsResult r;
    try {
        r.host = std::string{host};
        if (!is_valid_dns_host(host)) {
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
                    const int code = resolver(name);
                    const std::chrono::duration<double, std::milli> took =
                        std::chrono::steady_clock::now() - start;
                    promise.set_value({.code = code, .duration_ms = took.count()});
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
            r.error = ::gai_strerror(outcome.code);
            return r;
        }
        r.status = core::Status::ok;
        r.duration_ms = outcome.duration_ms;
    } catch (const std::exception& e) {
        r.status = core::Status::error;
        r.error = std::string{"cannot run the lookup: "} + e.what();
    }
    return r;
}

}  // namespace meerkat::probe
