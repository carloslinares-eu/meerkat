/**
 * @file pause_file.cpp
 * @brief Implementation of the pause file.
 */
#include "store/pause_file.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>

#include "store/append_file.hpp"
#include "sys/regular_file.hpp"

namespace meerkat::store {


std::optional<core::Timestamp> read_pause(const std::filesystem::path& data_dir) {
    const auto path = data_dir / kPauseFile;
    std::string content;
    try {
        // Never blocks: a FIFO in its place would hang every reader (#75).
        content = sys::read_regular_file(path, 64);
    } catch (const std::system_error& e) {
        if (e.code() == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }
        throw;
    }
    std::string_view text{content};
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }
    const auto until = core::parse_iso8601(text);
    if (!until) {
        throw std::runtime_error(path.string() + " doesn't hold a time; `meerkat pause` or "
                                 "`meerkat resume` replaces it");
    }
    return until;
}

void write_pause(const std::filesystem::path& data_dir, core::Timestamp until) {
    const auto target = data_dir / kPauseFile;
    const auto tmp = data_dir / ("." + std::string{kPauseFile} + "." +
                                 std::to_string(::getpid()) + ".tmp");
    ::unlink(tmp.c_str());  // left by a crashed process that had our pid
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "can't create " + tmp.string());
    }
    const bool written = write_all(fd, core::to_iso8601(until) + "\n") && ::fsync(fd) == 0;
    const int write_err = errno;
    const bool closed = ::close(fd) == 0;
    const int close_err = errno;
    if (!written || !closed) {
        ::unlink(tmp.c_str());
        throw std::system_error(written ? close_err : write_err, std::generic_category(),
                                "can't write " + tmp.string());
    }
    // rename() replaces a symlink itself, never its target.
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        throw std::system_error(err, std::generic_category(), "can't write " + target.string());
    }
    sync_parent_dir(target);
}

bool remove_pause(const std::filesystem::path& data_dir) {
    const auto path = data_dir / kPauseFile;
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
