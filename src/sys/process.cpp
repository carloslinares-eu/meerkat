/**
 * @file process.cpp
 * @brief Implementation of sys::run() and sys::find_executable().
 */
#include "sys/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <system_error>

/// The process environment (POSIX). Not declared by <unistd.h> in strict C++ mode.
extern char** environ;  // NOLINT

namespace meerkat::sys {

namespace {

using Clock = std::chrono::steady_clock;

/// How long output pipes may stay open after the program itself has exited.
constexpr std::chrono::milliseconds kDrainGrace{500};
/// Longest single wait in the poll loop when no pidfd is available; bounds
/// how late an exit is noticed on kernels older than 5.3.
constexpr std::chrono::milliseconds kTick{50};
/// Upper bound for Command::timeout and Command::kill_grace, so adding them to
/// a steady_clock time point can't overflow.
constexpr std::chrono::milliseconds kMaxWait{std::chrono::hours{24 * 365}};

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

/**
 * @brief Creates a close-on-exec pipe.
 * @param read_end  Receives the read end.
 * @param write_end Receives the write end.
 * @throws std::system_error if pipe2() fails.
 */
void make_pipe(Fd& read_end, Fd& write_end) {
    std::array<int, 2> fds{};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }
    read_end = Fd{fds[0]};
    write_end = Fd{fds[1]};
}

/// Converts a steady-clock duration to whole milliseconds, rounding up.
std::chrono::milliseconds ceil_ms(Clock::duration d) {
    return std::chrono::ceil<std::chrono::milliseconds>(d);
}

/**
 * @brief Reads what is available on a pipe into @p buf, keeping at most
 *        @p max bytes.
 * @param fd        The pipe's read end; reset on EOF or error.
 * @param buf       Output buffer.
 * @param max       Maximum bytes to keep.
 * @param truncated Set when bytes had to be dropped.
 */
void drain(Fd& fd, std::string& buf, std::size_t max, bool& truncated) {
    std::array<char, 4096> chunk{};
    const ssize_t n = ::read(fd.get(), chunk.data(), chunk.size());
    if (n > 0) {
        const auto got = static_cast<std::size_t>(n);
        const std::size_t room = buf.size() < max ? max - buf.size() : 0;
        buf.append(chunk.data(), got < room ? got : room);
        if (got > room) {
            truncated = true;
        }
    } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
        fd.reset();
    }
}

/**
 * @brief Reaps a child process, and kills its process group if it is still
 *        running when the guard goes out of scope (e.g. on an exception).
 */
class ChildGuard {
public:
    /// @brief Guards @p pid, which leads its own process group. @param pid The child.
    explicit ChildGuard(pid_t pid) : pid_{pid} {}
    ChildGuard(const ChildGuard&) = delete;
    ChildGuard& operator=(const ChildGuard&) = delete;
    ~ChildGuard() {
        if (!reaped_) {
            ::kill(-pid_, SIGKILL);
            reap();
        }
    }

    /**
     * @brief Reaps the child without blocking.
     * @return True if the child has been reaped (now or before).
     * @throws std::system_error for waitpid errors other than EINTR/ECHILD.
     */
    bool try_reap() { return wait(WNOHANG); }

    /// @brief Blocks until the child is reaped. Never throws.
    void reap() noexcept {
        try {
            while (!wait(0)) {
            }
        } catch (...) {
            reaped_ = true;  // nothing more can be done
        }
    }

    /// @brief Whether the child has been reaped. @return True once reaped.
    [[nodiscard]] bool reaped() const { return reaped_; }
    /// @brief Whether status() is the real wait status. @return False if reaped elsewhere.
    [[nodiscard]] bool status_known() const { return known_; }
    /// @brief The wait status from waitpid(). @return The raw status.
    [[nodiscard]] int status() const { return status_; }

private:
    /**
     * @brief One waitpid() call, retried on EINTR.
     * @param flags 0 or WNOHANG.
     * @return True if the child is reaped.
     */
    bool wait(int flags) {
        if (reaped_) {
            return true;
        }
        while (true) {
            const pid_t w = ::waitpid(pid_, &status_, flags);
            if (w == pid_) {
                reaped_ = known_ = true;
                return true;
            }
            if (w == 0) {
                return false;  // WNOHANG: still running
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                reaped_ = true;  // reaped by someone else; status lost
                return true;
            }
            throw std::system_error(errno, std::generic_category(), "waitpid");
        }
    }

    pid_t pid_;            ///< The child (and its process group id).
    int status_ = 0;       ///< Wait status once reaped.
    bool reaped_ = false;  ///< The child no longer exists.
    bool known_ = false;   ///< status_ came from our own waitpid().
};

/**
 * @brief Code that runs in the child between fork() and exec().
 *
 * Only async-signal-safe calls are allowed here, because the parent may have
 * other threads.
 */
[[noreturn]] void child_exec(const char* path, char* const* argv, char* const* envp, int in_fd,
                             int out_fd, int err_fd, int report_fd, pid_t parent) {
    ::setpgid(0, 0);
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != parent) {
        ::_exit(127);  // the parent already died before PDEATHSIG was armed
    }
    // Start the program with default signal handling and nothing blocked.
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    for (int s = 1; s < NSIG; ++s) {
        if (s != SIGKILL && s != SIGSTOP) {
            ::signal(s, SIG_DFL);
        }
    }
    // dup2(fd, fd) is a no-op that keeps O_CLOEXEC, which happens when meerkat
    // was started with fd 0/1/2 closed; clear the flag explicitly then.
    const auto to_std = [](int from, int to) {
        return from == to ? ::fcntl(to, F_SETFD, 0) : ::dup2(from, to);
    };
    if (to_std(in_fd, STDIN_FILENO) < 0 || to_std(out_fd, STDOUT_FILENO) < 0 ||
        to_std(err_fd, STDERR_FILENO) < 0) {
        const int e = errno;
        [[maybe_unused]] auto w = ::write(report_fd, &e, sizeof e);
        ::_exit(127);
    }
    ::execve(path, argv, envp);
    const int e = errno;
    [[maybe_unused]] auto w = ::write(report_fd, &e, sizeof e);
    ::_exit(127);
}

}  // namespace

std::optional<std::filesystem::path> find_executable(
    std::string_view name, std::string_view path_env,
    const std::vector<std::filesystem::path>& extra_dirs) {
    const auto usable = [](const std::filesystem::path& p) {
        struct stat st {};
        return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
               ::access(p.c_str(), X_OK) == 0;
    };
    if (name.empty()) {
        return std::nullopt;
    }
    if (name.find('/') != std::string_view::npos) {
        std::filesystem::path p{name};
        return usable(p) ? std::optional{p} : std::nullopt;
    }
    std::vector<std::filesystem::path> dirs;
    while (true) {
        const auto colon = path_env.find(':');
        const auto dir = path_env.substr(0, colon);
        // An empty entry means "current directory" to a shell; never run
        // programs from wherever meerkat happens to be started.
        if (!dir.empty()) {
            dirs.emplace_back(dir);
        }
        if (colon == std::string_view::npos) {
            break;
        }
        path_env.remove_prefix(colon + 1);
    }
    dirs.insert(dirs.end(), extra_dirs.begin(), extra_dirs.end());
    for (const auto& dir : dirs) {
        auto candidate = dir / name;
        if (usable(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

ProcessResult run(const Command& cmd) {
    ProcessResult r;
    const auto start = Clock::now();
    const auto finish = [&]() -> ProcessResult& {
        r.duration = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        return r;
    };

    const char* path_env = std::getenv("PATH");
    const auto exe = find_executable(cmd.program, path_env ? path_env : "/usr/bin:/bin");
    if (!exe) {
        r.outcome = ProcessResult::Outcome::spawn_failed;
        r.not_found = true;
        r.error = "program not found: " + cmd.program;
        return finish();
    }

    // Everything the child needs is prepared before fork(): it may not allocate.
    const std::string exe_str = exe->string();
    std::vector<std::string> arg_storage;
    arg_storage.reserve(cmd.args.size() + 1);
    arg_storage.push_back(cmd.program);
    arg_storage.insert(arg_storage.end(), cmd.args.begin(), cmd.args.end());
    std::vector<char*> argv;
    for (auto& a : arg_storage) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string_view entry{*e};
        const auto key = entry.substr(0, entry.find('='));
        bool overridden = false;
        for (const auto& [k, v] : cmd.env) {
            overridden = overridden || k == key;
        }
        if (!overridden) {
            env_storage.emplace_back(entry);
        }
    }
    for (const auto& [k, v] : cmd.env) {
        env_storage.push_back(k + "=" + v);
    }
    std::vector<char*> envp;
    for (auto& e : env_storage) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    Fd dev_null{::open("/dev/null", O_RDONLY | O_CLOEXEC)};
    if (dev_null.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "open /dev/null");
    }
    Fd out_r, out_w, err_r, err_w, rep_r, rep_w;
    make_pipe(out_r, out_w);
    make_pipe(err_r, err_w);
    make_pipe(rep_r, rep_w);

    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        throw std::system_error(errno, std::generic_category(), "fork");
    }
    if (pid == 0) {
        child_exec(exe_str.c_str(), argv.data(), envp.data(), dev_null.get(), out_w.get(),
                   err_w.get(), rep_w.get(), parent);
    }
    ::setpgid(pid, pid);  // also done in the child; whichever runs first wins
    out_w.reset();
    err_w.reset();
    rep_w.reset();
    dev_null.reset();

    // From here on the child must never be left behind: if anything below
    // throws, the guard kills the whole group and reaps the child.
    ChildGuard guard{pid};

    // exec() closes the report pipe (O_CLOEXEC); anything readable is an errno.
    int exec_errno = 0;
    ssize_t n = 0;
    do {
        n = ::read(rep_r.get(), &exec_errno, sizeof exec_errno);
    } while (n < 0 && errno == EINTR);
    if (n == static_cast<ssize_t>(sizeof exec_errno)) {
        guard.reap();
        r.outcome = ProcessResult::Outcome::spawn_failed;
        r.not_found = exec_errno == ENOENT;
        r.error = "cannot run " + exe_str + ": " + std::strerror(exec_errno);
        return finish();
    }

    // A pidfd becomes readable the moment the child exits (Linux >= 5.3).
    // Without one, the loop falls back to checking every kTick.
    const Fd pidfd{static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))};

    const auto timeout = std::clamp(cmd.timeout, std::chrono::milliseconds{0}, kMaxWait);
    const auto kill_grace = std::clamp(cmd.kill_grace, std::chrono::milliseconds{0}, kMaxWait);
    auto deadline = start + timeout;
    bool term_sent = false, kill_sent = false;
    std::optional<Clock::time_point> drain_deadline;

    while (out_r.get() >= 0 || err_r.get() >= 0 || !guard.reaped()) {
        // Reap first, so a program that exited just before the deadline is
        // never reported as timed out.
        if (!guard.reaped() && guard.try_reap()) {
            drain_deadline = Clock::now() + kDrainGrace;
        }
        if (guard.reaped() && out_r.get() < 0 && err_r.get() < 0) {
            break;  // nothing left to wait for
        }
        const auto now = Clock::now();

        if (!guard.reaped() && now >= deadline) {
            if (!term_sent) {
                ::kill(-pid, SIGTERM);
                term_sent = true;
                deadline = now + kill_grace;
            } else if (!kill_sent) {
                ::kill(-pid, SIGKILL);
                kill_sent = true;
                deadline = Clock::time_point::max();
            }
        }
        if (guard.reaped() && drain_deadline && now >= *drain_deadline) {
            // The program is gone but something it started still holds our pipes.
            ::kill(-pid, SIGKILL);
            out_r.reset();
            err_r.reset();
            break;
        }

        std::array<pollfd, 3> fds{{{out_r.get(), POLLIN, 0},
                                   {err_r.get(), POLLIN, 0},
                                   {guard.reaped() ? -1 : pidfd.get(), POLLIN, 0}}};
        // Without a pidfd, wake up regularly to check for the exit. With one,
        // only the deadlines bound the wait.
        auto wait = (pidfd.get() >= 0 || guard.reaped()) ? kMaxWait : kTick;
        if (!guard.reaped() && deadline != Clock::time_point::max()) {
            wait = std::min(wait, ceil_ms(deadline - now));
        }
        if (guard.reaped() && drain_deadline) {
            wait = std::min(wait, ceil_ms(*drain_deadline - now));
        }
        wait = std::max(wait, std::chrono::milliseconds{0});
        const int ready = ::poll(fds.data(), fds.size(), static_cast<int>(std::min<long long>(
                                                             wait.count(), 1'000'000)));
        if (ready < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        if (ready > 0) {
            if (fds[0].revents != 0) {
                drain(out_r, r.out, cmd.max_output, r.out_truncated);
            }
            if (fds[1].revents != 0) {
                drain(err_r, r.err, cmd.max_output, r.err_truncated);
            }
        }
    }

    const int status = guard.status();
    if (!guard.status_known()) {
        // Someone else reaped the child (SIGCHLD ignored, or a waitpid(-1)
        // elsewhere): it is gone, but how it ended is unknown.
        r.outcome = ProcessResult::Outcome::exited;
        r.exit_code = -1;
        r.error = "exit status unavailable: the child was reaped elsewhere";
    } else if (term_sent) {
        r.outcome = ProcessResult::Outcome::timed_out;
        r.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } else if (WIFSIGNALED(status)) {
        r.outcome = ProcessResult::Outcome::signaled;
        r.signal = WTERMSIG(status);
    } else {
        r.outcome = ProcessResult::Outcome::exited;
        r.exit_code = WEXITSTATUS(status);
    }
    return finish();
}

}  // namespace meerkat::sys
