/**
 * @file privileges.hpp
 * @brief Giving up root before working in a directory another user owns.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <sys/types.h>

namespace meerkat::sys {

/// A user and group to become.
struct Identity {
    uid_t uid = 0;  ///< The user.
    gid_t gid = 0;  ///< The group.
};

/**
 * @brief Whom a root process should become before working in @p dir: its
 *        owner and group, unless that is root or this process isn't root.
 *
 * Root following paths in a directory a less-privileged user controls
 * would follow that user's symlinks (#133); as the owner it can do nothing
 * the owner couldn't do anyway.
 * @param dir The directory; it must exist.
 * @return The identity to drop to; empty if nothing needs dropping.
 * @throws std::system_error if @p dir can't be examined.
 */
std::optional<Identity> owner_to_become(const std::filesystem::path& dir);

/**
 * @brief Becomes @p who for good: no supplementary groups, then the group,
 *        then the user (real, effective and saved), and checks that root
 *        can't be regained.
 * @param who The identity.
 * @throws std::system_error if any step fails.
 */
void become(const Identity& who);

}  // namespace meerkat::sys
