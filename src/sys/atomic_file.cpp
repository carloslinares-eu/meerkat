/**
 * @file atomic_file.cpp
 * @brief Implementation of sys::AtomicFile.
 */
#include "sys/atomic_file.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

namespace meerkat::sys {

AtomicFile::AtomicFile(std::filesystem::path path, unsigned mode) : path_{std::move(path)} {
    tmp_ = path_.parent_path() / ("." + path_.filename().string() + ".tmp-" + std::to_string(::getpid()));
    ::unlink(tmp_.c_str());  // left over from a crashed run with our pid
    fd_ = ::open(tmp_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "can't create " + tmp_.string());
    }
}

AtomicFile::~AtomicFile() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    if (!done_) {
        ::unlink(tmp_.c_str());
    }
}

void AtomicFile::write(std::string_view data) {
    while (!data.empty()) {
        const auto n = ::write(fd_, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "can't write " + tmp_.string());
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

void AtomicFile::commit(bool strict) {
    if (::fsync(fd_) != 0) {
        throw std::system_error(errno, std::generic_category(), "can't write " + tmp_.string());
    }
    const int fd = std::exchange(fd_, -1);
    if (::close(fd) != 0) {
        throw std::system_error(errno, std::generic_category(), "can't write " + tmp_.string());
    }
    if (::rename(tmp_.c_str(), path_.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "can't write " + path_.string());
    }
    done_ = true;
    // The rename only lasts a power cut once the directory is on disk too.
    const auto dir = path_.parent_path().empty() ? std::filesystem::path{"."} : path_.parent_path();
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const bool synced = dfd >= 0 && ::fsync(dfd) == 0;
    const int err = errno;
    if (dfd >= 0) {
        ::close(dfd);
    }
    if (strict && !synced) {
        throw std::system_error(err, std::generic_category(), "can't sync the directory of " + path_.string());
    }
}

}  // namespace meerkat::sys
