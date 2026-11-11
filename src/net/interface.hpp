/**
 * @file interface.hpp
 * @brief Validation of network interface names.
 */
#pragma once

#include <string_view>

namespace meerkat::net {

/**
 * @brief Checks @p name against the kernel's rules for interface names.
 *
 * Same rules as the kernel's `dev_valid_name()`: 1–15 bytes, not `.` or
 * `..`, and no `/`, `:` or whitespace. Anything the kernel accepts is accepted,
 * so no real interface is ever skipped; anything else (path tricks, spaces)
 * is refused before a name goes into a sysfs path or a tool's arguments.
 *
 * @param name The candidate name.
 * @return True if it is a valid interface name.
 */
bool is_valid_interface_name(std::string_view name);

}  // namespace meerkat::net
