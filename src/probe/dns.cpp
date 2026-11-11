/**
 * @file dns.cpp
 * @brief Implementation of the DNS probe.
 */
#include "probe/dns.hpp"

#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace meerkat::probe {

namespace {

/// State shared between the caller and the lookup thread.
struct Lookup {
    std::mutex mutex;              ///< Guards the fields below.
    std::condition_variable done;  ///< Signalled when the lookup finishes.
    std::optional<int> code;       ///< getaddrinfo() result once finished.
    std::string failure;           ///< Set if the resolver threw.
};

}  // namespace

int system_resolve(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (result != nullptr) {
        ::freeaddrinfo(result);
    }
    return rc;
}

bool is_valid_dns_host(std::string_view host) {
    return !host.empty() && host.size() <= 253 &&
           std::none_of(host.begin(), host.end(), [](char c) {
               return static_cast<unsigned char>(c) <= ' ' || c == '\x7f';
           });
}

core::DnsResult resolve(std::string_view host, std::chrono::milliseconds timeout,
                        const Resolver& resolver) {
    using Clock = std::chrono::steady_clock;
    core::DnsResult r;
    r.host = std::string{host};
    if (!is_valid_dns_host(host)) {
        r.error = "invalid host name";
        return r;
    }

    auto state = std::make_shared<Lookup>();
    const auto start = Clock::now();
    try {
        // The thread gets its own copies of everything it touches, so it can
        // outlive this call safely if the lookup hangs.
        std::thread([state, name = r.host, resolver]() {
            int code = EAI_FAIL;
            std::string failure;
            try {
                code = resolver(name);
            } catch (const std::exception& e) {
                failure = e.what();
            } catch (...) {
                failure = "unknown exception";
            }
            const std::lock_guard lock{state->mutex};
            state->code = code;
            state->failure = std::move(failure);
            state->done.notify_all();
        }).detach();
    } catch (const std::exception& e) {
        r.error = std::string{"cannot start the lookup: "} + e.what();
        return r;
    }

    std::unique_lock lock{state->mutex};
    const bool finished = state->done.wait_for(lock, std::max(timeout, std::chrono::milliseconds{0}),
                                               [&] { return state->code.has_value(); });
    const auto elapsed = Clock::now() - start;
    if (!finished) {
        r.status = core::Status::timeout;
        r.error = "no answer within " + std::to_string(timeout.count()) + " ms";
        return r;
    }
    if (!state->failure.empty()) {
        r.error = "resolver failed: " + state->failure;
        return r;
    }
    if (*state->code != 0) {
        r.error = ::gai_strerror(*state->code);
        return r;
    }
    r.status = core::Status::ok;
    r.duration_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    return r;
}

}  // namespace meerkat::probe
