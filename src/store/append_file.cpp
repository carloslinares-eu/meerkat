/**
 * @file append_file.cpp
 * @brief Implementation of AppendFile.
 */
#include "store/append_file.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace meerkat::store {

namespace {

/**
 * @brief Builds a std::system_error from the current errno.
 *
 * errno is read before the message is built, since building it may allocate.
 *
 * @param what Description of the failed step.
 * @param path The file involved.
 * @return The exception.
 */
std::system_error os_error(std::string_view what, const std::filesystem::path& path) {
    const int err = errno;
    return {err, std::generic_category(), std::string{what} + " " + path.string()};
}

}  // namespace

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

void sync_parent_dir(const std::filesystem::path& path) {
    auto dir = path.parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        throw os_error("can't open the directory of", path);
    }
    if (::fsync(dfd) != 0) {
        const auto e = os_error("can't sync the directory of", path);
        ::close(dfd);
        throw e;
    }
    ::close(dfd);
}

AppendFile::AppendFile(std::filesystem::path path) : path_{std::move(path)} {
    // O_EXCL first, so we know whether this call created the file. O_RDWR in
    // both cases: another process may have written to it before we got the
    // lock, and the caller reads the tail back.
    created_ = true;
    fd_ = ::open(path_.c_str(), O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    if (fd_ < 0 && errno == EEXIST) {
        created_ = false;
        // O_NOFOLLOW: a data file replaced by a symlink is refused, not
        // followed to wherever it points. (O_EXCL above never follows one.)
        fd_ = ::open(path_.c_str(), O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd_ < 0) {
        if (!created_ && errno == ELOOP) {  // the O_NOFOLLOW open: the file itself is a link
            throw std::system_error(ELOOP, std::generic_category(),
                                    "a symlink, not a regular file: " + path_.string());
        }
        throw os_error("can't open", path_);
    }
    try {
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            const int err = errno;
            if (err == EWOULDBLOCK) {
                throw std::system_error(err, std::generic_category(),
                                        "another process is writing " + path_.string());
            }
            throw std::system_error(err, std::generic_category(), "can't lock " + path_.string());
        }
        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            throw os_error("can't stat", path_);
        }
        if (!S_ISREG(st.st_mode)) {
            throw std::system_error(EINVAL, std::generic_category(),
                                    "not a regular file: " + path_.string());
        }
        if (created_) {
            sync_parent_dir(path_);
        }
    } catch (...) {
        close();
        throw;
    }
}

AppendFile::~AppendFile() { close(); }

AppendFile::AppendFile(AppendFile&& other) noexcept
    : path_{std::move(other.path_)},
      fd_{std::exchange(other.fd_, -1)},
      created_{other.created_} {}

AppendFile& AppendFile::operator=(AppendFile&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        fd_ = std::exchange(other.fd_, -1);
        created_ = other.created_;
    }
    return *this;
}

void AppendFile::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void AppendFile::require_open(std::string_view what) const {
    if (fd_ < 0) {
        throw std::logic_error(std::string{what} + " on closed file " + path_.string() +
                               " (moved from, or closed by an earlier write error)");
    }
}

std::size_t AppendFile::size() const {
    require_open("size()");
    struct stat st {};
    if (::fstat(fd_, &st) != 0) {
        throw os_error("can't stat", path_);
    }
    return static_cast<std::size_t>(st.st_size);
}

std::string AppendFile::read(std::size_t offset, std::size_t count) const {
    require_open("read()");
    std::string out(count, '\0');
    std::size_t got = 0;
    while (got < count) {
        const ssize_t n = ::pread(fd_, out.data() + got, count - got,
                                  static_cast<off_t>(offset + got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw os_error("can't read", path_);
        }
        if (n == 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    out.resize(got);
    return out;
}

void AppendFile::append(std::string_view data) {
    require_open("append()");
    if (!write_all(fd_, data)) {
        const auto e = os_error("can't write to", path_);
        close();
        throw e;
    }
    if (::fdatasync(fd_) != 0) {
        const auto e = os_error("can't sync", path_);
        close();
        throw e;
    }
}

void AppendFile::rewrite(std::string_view data) {
    require_open("rewrite()");
    if (::ftruncate(fd_, 0) != 0) {
        const auto e = os_error("can't truncate", path_);
        close();
        throw e;
    }
    append(data);
}

}  // namespace meerkat::store
