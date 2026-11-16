/**
 * @file state_file.cpp
 * @brief Implementation of the state-file helpers.
 */
#include "store/state_file.hpp"

#include <unistd.h>

#include <cerrno>
#include <system_error>

#include "store/append_file.hpp"
#include "sys/regular_file.hpp"

namespace meerkat::store {

std::optional<std::string> read_state_file(const std::filesystem::path& path) {
    std::string content;
    try {
        content = sys::read_regular_file(path, 64);
    } catch (const std::system_error& e) {
        if (e.code() == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }
        throw;
    }
    if (!content.empty() && content.back() == '\n') {
        content.pop_back();
    }
    return content;
}

bool remove_state_file(const std::filesystem::path& path) {
    if (::unlink(path.c_str()) == 0) {
        sync_parent_dir(path);
        return true;
    }
    if (errno == ENOENT) {
        return false;
    }
    throw std::system_error(errno, std::generic_category(), "can't remove " + path.string());
}

}  // namespace meerkat::store
