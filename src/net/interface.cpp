/**
 * @file interface.cpp
 * @brief Implementation of net::is_valid_interface_name().
 */
#include "net/interface.hpp"

namespace meerkat::net {

bool is_valid_interface_name(std::string_view name) {
    if (name.empty() || name.size() > 15 || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        // isspace() in the C locale, plus the two characters the kernel forbids.
        if (c == '/' || c == ':' || c == ' ' || c == '\t' || c == '\n' || c == '\v' ||
            c == '\f' || c == '\r' || c == '\0') {
            return false;
        }
    }
    return true;
}

}  // namespace meerkat::net
