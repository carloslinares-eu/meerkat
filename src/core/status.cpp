/**
 * @file status.cpp
 * @brief Text forms of Status and LinkType.
 */
#include "core/status.hpp"

#include <array>
#include <utility>

namespace meerkat::core {

namespace {

/// Every Status with its stored name.
constexpr std::array<std::pair<Status, std::string_view>, 7> kStatusNames{{
    {Status::ok, "ok"},
    {Status::timeout, "timeout"},
    {Status::error, "error"},
    {Status::no_network, "no_network"},
    {Status::not_available, "not_available"},
    {Status::skipped_wlan, "skipped_wlan"},
    {Status::paused_official, "paused_official"},
}};

/// Every LinkType with its stored name.
constexpr std::array<std::pair<LinkType, std::string_view>, 3> kLinkTypeNames{{
    {LinkType::unknown, "unknown"},
    {LinkType::wired, "wired"},
    {LinkType::wifi, "wifi"},
}};

}  // namespace

std::string_view to_string(Status s) {
    for (const auto& [value, name] : kStatusNames) {
        if (value == s) {
            return name;
        }
    }
    return "error";
}

std::optional<Status> parse_status(std::string_view text) {
    for (const auto& [value, name] : kStatusNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::string_view to_string(LinkType t) {
    for (const auto& [value, name] : kLinkTypeNames) {
        if (value == t) {
            return name;
        }
    }
    return "unknown";
}

std::optional<LinkType> parse_link_type(std::string_view text) {
    for (const auto& [value, name] : kLinkTypeNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

}  // namespace meerkat::core
