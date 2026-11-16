/**
 * @file privileges.cpp
 * @brief Implementation of giving up root.
 */
#include "sys/privileges.hpp"

#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <system_error>

namespace meerkat::sys {

std::optional<Identity> owner_to_become(const std::filesystem::path& dir) {
    if (::geteuid() != 0) {
        return std::nullopt;
    }
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) {
        throw std::system_error(errno, std::generic_category(), "can't examine " + dir.string());
    }
    if (st.st_uid == 0) {
        return std::nullopt;  // root's own directory
    }
    return Identity{st.st_uid, st.st_gid};
}

void become(const Identity& who) {
    const auto fail = [](const std::string& what) {
        throw std::system_error(errno, std::generic_category(), "can't give up root: " + what);
    };
    if (::setgroups(0, nullptr) != 0) {
        fail("setgroups");
    }
    if (::setresgid(who.gid, who.gid, who.gid) != 0) {
        fail("setresgid");
    }
    if (::setresuid(who.uid, who.uid, who.uid) != 0) {
        fail("setresuid");
    }
    if (who.uid != 0 && (::setuid(0) == 0 || ::geteuid() == 0)) {
        errno = EPERM;
        fail("root could be regained");
    }
}

}  // namespace meerkat::sys
