/**
 * @file fd.hpp
 * @brief An owned file descriptor, closed on destruction.
 */
#pragma once

#include <unistd.h>

#include <utility>

namespace meerkat::sys {

/// Owns a file descriptor and closes it on destruction.
class Fd {
public:
    Fd() = default;
    /// @brief Takes ownership of @p fd. @param fd Descriptor, or -1.
    explicit Fd(int fd) : fd_{fd} {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    /// @brief Moves ownership. @param o Source; left empty.
    Fd(Fd&& o) noexcept : fd_{std::exchange(o.fd_, -1)} {}
    /// @brief Moves ownership. @param o Source; left empty. @return This.
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }
    /// @brief The descriptor, or -1. @return The descriptor.
    [[nodiscard]] int get() const { return fd_; }
    /// @brief Closes the descriptor if open.
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;  ///< Owned descriptor, or -1.
};

}  // namespace meerkat::sys
