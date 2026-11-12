/**
 * @file status.cpp
 * @brief Text forms of Status and LinkType.
 */
#include "core/status.hpp"

#include <type_traits>

namespace meerkat::core {

namespace {

/**
 * @brief Finds the enumerator whose to_string() equals @p text.
 *
 * Tries every value from 0 up to and including @p last, so it needs no table
 * of its own; to_string() (a switch the compiler checks) stays the only list
 * of names.
 *
 * @tparam E    The enum type.
 * @param last  The highest enumerator.
 * @param text  The text to look up.
 * @return The matching enumerator, or `std::nullopt`.
 */
template <typename E>
std::optional<E> parse_enum(E last, std::string_view text) {
    using U = std::underlying_type_t<E>;
    for (U i = 0; i <= static_cast<U>(last); ++i) {
        const auto value = static_cast<E>(i);
        if (to_string(value) == text) {
            return value;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string_view to_string(Status s) {
    // No default: -Wswitch (an error with -Werror) flags a missing name.
    switch (s) {
        case Status::ok: return "ok";
        case Status::timeout: return "timeout";
        case Status::error: return "error";
        case Status::no_network: return "no_network";
        case Status::not_available: return "not_available";
        case Status::skipped_wlan: return "skipped_wlan";
        case Status::paused_official: return "paused_official";
    }
    return "error";  // unreachable for valid enumerators
}

std::optional<Status> parse_status(std::string_view text) {
    return parse_enum(kLastStatus, text);
}

std::string_view to_string(LinkType t) {
    switch (t) {
        case LinkType::unknown: return "unknown";
        case LinkType::wired: return "wired";
        case LinkType::wifi: return "wifi";
    }
    return "unknown";  // unreachable for valid enumerators
}

std::optional<LinkType> parse_link_type(std::string_view text) {
    return parse_enum(kLastLinkType, text);
}

std::string status_text(Status status, const std::string& error) {
    std::string s{to_string(status)};
    if (!error.empty()) {
        s += ": " + error;
    }
    return s;
}

}  // namespace meerkat::core
