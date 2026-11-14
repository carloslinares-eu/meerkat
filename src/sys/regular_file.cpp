/**
 * @file regular_file.cpp
 * @brief Implementation of read_regular_file().
 */
#include "sys/regular_file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

#include "sys/fd.hpp"

namespace meerkat::sys {

Fd open_regular_file(const std::filesystem::path& path) {
    Fd fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY)};
    if (fd.get() < 0) {
        const int err = errno;
        throw std::system_error(err, std::generic_category(), "can't read " + path.string());
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        const int err = errno;
        throw std::system_error(err, std::generic_category(), "can't stat " + path.string());
    }
    if (!S_ISREG(st.st_mode)) {
        throw NotRegularFile(path.string() + " isn't a regular file");
    }
    return fd;
}

std::string read_regular_file(const std::filesystem::path& path, std::size_t max) {
    const Fd fd = open_regular_file(path);
    std::string data(max, '\0');
    std::size_t got = 0;
    while (got < max) {
        const ssize_t n = ::read(fd.get(), data.data() + got, max - got);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            const int err = errno;
            throw std::system_error(err, std::generic_category(), "can't read " + path.string());
        }
        if (n == 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    data.resize(got);
    return data;
}

}  // namespace meerkat::sys
