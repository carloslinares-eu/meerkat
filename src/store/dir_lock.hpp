/**
 * @file dir_lock.hpp
 * @brief An exclusive lock on a whole data directory: one meerkat at a time.
 */
#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace meerkat::store {

/**
 * @brief Locks a directory exclusively with `flock()` on the directory
 *        itself, for as long as the object lives.
 *
 * The lock sits on the directory's inode, not on a lock file, so it can't
 * be deleted or replaced while it is held, it needs only read access to the
 * directory, and nothing is left behind in it. The descriptor is
 * close-on-exec, so child processes (`ping`, `speedtest`) don't keep the
 * lock alive.
 *
 * Some network file systems (NFS) can't take an exclusive `flock()` on a
 * descriptor opened read-only; the state is then `unsupported` and the
 * caller decides (the recorder warns and relies on the per-file locks of
 * its data files, as before).
 */
class DirectoryLock {
public:
    /// What trying to lock found.
    enum class State {
        held,         ///< Locked by this object.
        busy,         ///< Someone else holds it: another process or another DirectoryLock.
        unsupported,  ///< The file system can't lock it (see error()).
        released,     ///< Holds nothing any more: moved from.
    };

    /**
     * @brief Locks @p dir, trying again for up to @p patience while someone
     *        else holds it.
     * @param dir      The directory; it must exist.
     * @param patience How long to keep trying while the lock is busy; zero
     *                 tries once. A holder that lets go at once, such as
     *                 `meerkat check` testing the lock, is then waited for.
     * @throws std::system_error if @p dir can't be opened as a directory.
     */
    explicit DirectoryLock(const std::filesystem::path& dir,
                           std::chrono::milliseconds patience = std::chrono::milliseconds{0});

    /// Releases the lock.
    ~DirectoryLock();

    DirectoryLock(const DirectoryLock&) = delete;             ///< Owns a descriptor: not copyable.
    DirectoryLock& operator=(const DirectoryLock&) = delete;  ///< Owns a descriptor: not copyable.

    /**
     * @brief Takes over @p other's lock.
     * @param other The lock to move from; its state() becomes `released`.
     */
    DirectoryLock(DirectoryLock&& other) noexcept;

    /**
     * @brief Releases this lock and takes over @p other's.
     * @param other The lock to move from; its state() becomes `released`.
     * @return This object.
     */
    DirectoryLock& operator=(DirectoryLock&& other) noexcept;

    /// @return Whether the lock is held, busy, unsupported or released.
    [[nodiscard]] State state() const { return state_; }

    /// @return Why the lock is `unsupported` (the `flock()` error); empty otherwise.
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    /// Closes the descriptor, which releases the lock.
    void close() noexcept;

    int fd_ = -1;                    ///< The directory, open while the lock is held.
    State state_ = State::released;  ///< What trying to lock found.
    std::string error_;              ///< The `flock()` error for `unsupported`.
};

}  // namespace meerkat::store
