/**
 * @file dir_lock.cpp
 * @brief Implementation of store::DirectoryLock.
 */
#include "store/dir_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <thread>
#include <utility>

namespace meerkat::store {

namespace {

/// How often a busy lock is tried again while there is patience left.
constexpr std::chrono::milliseconds kRetryStep{10};

}  // namespace

DirectoryLock::DirectoryLock(const std::filesystem::path& dir, std::chrono::milliseconds patience) {
    fd_ = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "can't open " + dir.string());
    }
    const auto deadline = std::chrono::steady_clock::now() + patience;
    int err = 0;
    for (;;) {
        if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
            state_ = State::held;
            return;
        }
        err = errno;
        const auto now = std::chrono::steady_clock::now();
        if (err != EWOULDBLOCK || now >= deadline) {
            break;
        }
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(kRetryStep, deadline - now));
    }
    close();
    if (err == EWOULDBLOCK) {
        state_ = State::busy;
        return;
    }
    // EBADF (NFS emulates flock() with byte-range locks, and an exclusive one
    // needs a descriptor open for writing, which a directory can't have),
    // ENOLCK, EINVAL, … The data files are open for writing, so their own
    // locks (AppendFile) still work there; this lock is the extra one.
    state_ = State::unsupported;
    error_ = std::generic_category().message(err);  // thread-safe, unlike strerror
}

DirectoryLock::~DirectoryLock() { close(); }

DirectoryLock::DirectoryLock(DirectoryLock&& other) noexcept
    : fd_{std::exchange(other.fd_, -1)},
      state_{std::exchange(other.state_, State::released)},
      error_{std::exchange(other.error_, {})} {}

DirectoryLock& DirectoryLock::operator=(DirectoryLock&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        state_ = std::exchange(other.state_, State::released);
        error_ = std::exchange(other.error_, {});
    }
    return *this;
}

void DirectoryLock::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace meerkat::store
