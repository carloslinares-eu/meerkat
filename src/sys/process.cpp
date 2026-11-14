/**
 * @file process.cpp
 * @brief Implementation of sys::run() and sys::find_executable().
 */
#include "sys/process.hpp"

#include "sys/fd.hpp"
#include "core/text.hpp"

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
#include <mutex>
#include <ranges>
#include <set>
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
/// How long run() waits for a program to die after SIGKILL before it gives
/// up on it (a process stuck in the kernel can't die until it returns).
constexpr std::chrono::milliseconds kKillWait{5'000};
/// Upper bound for Command::timeout and Command::kill_grace, so adding them to
/// a steady_clock time point can't overflow.
constexpr std::chrono::milliseconds kMaxWait{std::chrono::hours{24 * 365}};

/**
 * @brief Creates a close-on-exec pipe whose read end (ours) doesn't block;
 *        the write end (the child's) stays blocking.
 * @param read_end  Receives the read end.
 * @param write_end Receives the write end.
 * @throws std::system_error if pipe2() or fcntl() fails.
 */
void make_pipe(Fd& read_end, Fd& write_end) {
    std::array<int, 2> fds{};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }
    read_end = Fd{fds[0]};
    write_end = Fd{fds[1]};
    if (::fcntl(read_end.get(), F_SETFL, O_NONBLOCK) != 0) {  // a new pipe has no other flags
        throw std::system_error(errno, std::generic_category(), "fcntl O_NONBLOCK");
    }
}

/// Converts a steady-clock duration to whole milliseconds, rounding up.
std::chrono::milliseconds ceil_ms(Clock::duration d) {
    return std::chrono::ceil<std::chrono::milliseconds>(d);
}

/// Most bytes drain() reads per call, so a program that writes without
/// pause can't keep run() from checking its deadlines.
constexpr std::size_t kDrainChunk = 64 * 1024;

/**
 * @brief Reads what is available on a non-blocking pipe into @p buf (up to
 *        kDrainChunk bytes per call), keeping at most @p max bytes.
 * @param fd        The pipe's read end; reset on EOF or error.
 * @param buf       Output buffer.
 * @param max       Maximum bytes to keep.
 * @param truncated Set when bytes had to be dropped.
 */
void drain(Fd& fd, std::string& buf, std::size_t max, bool& truncated) {
    std::array<char, 4096> chunk{};
    for (std::size_t total = 0; total < kDrainChunk;) {
        const ssize_t n = ::read(fd.get(), chunk.data(), chunk.size());
        if (n > 0) {
            const auto got = static_cast<std::size_t>(n);
            total += got;
            const std::size_t room = buf.size() < max ? max - buf.size() : 0;
            buf.append(chunk.data(), got < room ? got : room);
            if (got > room) {
                truncated = true;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n == 0 || errno != EAGAIN) {
            fd.reset();  // EOF, or an error: nothing more to read
        }
        return;
    }
}

/// Guards g_children. fork() runs under it, so a child is listed before
/// kill_running_children() can look.
std::mutex g_children_mutex;
/// Process groups of children that haven't been reaped yet.
std::set<pid_t> g_children;

/**
 * @brief Adds or removes a child (its process group) in g_children; the
 *        caller holds g_children_mutex.
 * @param pid     The child.
 * @param running True when started, false before it is reaped.
 */
void track_child_locked(pid_t pid, bool running) noexcept {
    try {
        if (running) {
            g_children.insert(pid);
        } else {
            g_children.erase(pid);
        }
    } catch (...) {
        // Out of memory while tracking: only kill_running_children() misses it.
    }
}

/**
 * @brief Removes a child from g_children.
 * @param pid The child.
 */
void untrack_child(pid_t pid) noexcept {
    std::lock_guard lock{g_children_mutex};
    track_child_locked(pid, false);
}

/**
 * @brief Watches a child process and reaps it; kills its process group if it
 *        is still running when the guard goes out of scope (an exception).
 *
 * The exit is noticed with `waitid(WNOWAIT)`, which leaves the child a
 * zombie: its pid, which is also its process group id, stays taken until
 * reap(). So kill_group() can never hit an unrelated process that got the
 * pid after the child was gone.
 */
class ChildGuard {
public:
    /**
     * @brief Guards @p pid, which leads its own process group and is
     *        already in g_children.
     * @param pid The child.
     */
    explicit ChildGuard(pid_t pid) : pid_{pid} {}
    ChildGuard(const ChildGuard&) = delete;             ///< Owns the child: not copyable.
    ChildGuard& operator=(const ChildGuard&) = delete;  ///< Owns the child: not copyable.
    /// Kills the group if the child is still running, and reaps it.
    ~ChildGuard() {
        if (!exited_ && !abandoned_) {
            kill_group(SIGKILL);
        }
        reap();
    }

    /**
     * @brief Checks without blocking whether the child has exited (it is
     *        left a zombie until reap()).
     * @return True once it has exited.
     * @throws std::system_error for waitid errors other than EINTR/ECHILD.
     */
    bool try_exited() {
        while (!exited_) {
            siginfo_t info{};
            if (::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
                exited_ = info.si_pid == pid_;  // si_pid stays 0 while it runs
                return exited_;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                // Reaped by someone else (SIGCHLD ignored): its pid may
                // already belong to another process, so no more kills.
                exited_ = reaped_ = lost_ = true;
                untrack_child(pid_);
                return true;
            }
            throw std::system_error(errno, std::generic_category(), "waitid");
        }
        return true;
    }

    /// @brief Whether the child has exited. @return True once it has.
    [[nodiscard]] bool exited() const { return exited_; }

    /**
     * @brief Sends @p sig to the child's process group, unless the child was
     *        reaped elsewhere (then the group id may be reused).
     * @param sig The signal.
     */
    void kill_group(int sig) const noexcept {
        if (!lost_) {
            ::kill(-pid_, sig);
        }
    }

    /**
     * @brief Blocks until the child has exited, then reaps it. Never throws.
     *
     * The zombie is reaped and removed from g_children under
     * g_children_mutex: kill_running_children() sees the group as long as
     * the pid is ours, and never after it is free for reuse.
     */
    void reap() noexcept {
        if (reaped_ || abandoned_) {
            return;
        }
        while (!exited_) {
            siginfo_t info{};
            if (::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOWAIT) == 0) {
                exited_ = true;
            } else if (errno != EINTR) {
                break;  // ECHILD: reaped elsewhere; waitpid() below says so
            }
        }
        std::lock_guard lock{g_children_mutex};
        while (true) {
            if (::waitpid(pid_, &status_, 0) == pid_) {
                known_ = true;
                break;
            }
            if (errno != EINTR) {
                wait_errno_ = errno;  // ECHILD: reaped elsewhere; status lost
                break;
            }
        }
        track_child_locked(pid_, false);
        reaped_ = exited_ = true;
    }

    /**
     * @brief Gives up on a child that doesn't die even of SIGKILL (stuck in
     *        the kernel, e.g. exec() from a hung NFS mount): it stays in
     *        g_children and unreaped, so its pid stays ours, and the guard
     *        no longer waits for it.
     */
    void abandon() noexcept { abandoned_ = true; }

    /// @brief Why waitpid() failed, if it did. @return The errno, or 0.
    [[nodiscard]] int wait_errno() const { return wait_errno_; }

    /// @brief Whether status() is the real wait status. @return False if reaped elsewhere.
    [[nodiscard]] bool status_known() const { return known_; }
    /// @brief The wait status from waitpid(). @return The raw status.
    [[nodiscard]] int status() const { return status_; }

private:
    pid_t pid_;            ///< The child (and its process group id).
    int status_ = 0;       ///< Wait status once reaped.
    bool exited_ = false;  ///< The child has exited (a zombie, or reaped).
    bool reaped_ = false;  ///< The child no longer exists.
    bool known_ = false;   ///< status_ came from our own waitpid().
    bool lost_ = false;    ///< Reaped by someone else before we saw it exit.
    bool abandoned_ = false;  ///< Given up on: never waited for.
    int wait_errno_ = 0;      ///< errno of a failed waitpid().
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
    // Handlers first: a signal arriving once unblocked must not run one of
    // meerkat's handlers in the child.
    for (int s = 1; s < NSIG; ++s) {
        if (s != SIGKILL && s != SIGSTOP) {
            ::signal(s, SIG_DFL);
        }
    }
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
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

void kill_running_children() noexcept {
    std::lock_guard lock{g_children_mutex};
    for (const pid_t pid : g_children) {
        ::kill(-pid, SIGKILL);
    }
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
        const bool overridden = std::ranges::any_of(
            cmd.env | std::views::keys, [&key](const std::string& k) { return k == key; });
        if (!overridden) {
            env_storage.emplace_back(entry);
        }
    }
    for (const auto& [k, v] : cmd.env) {
        std::string entry = k;
        entry += '=';
        entry += v;
        env_storage.push_back(std::move(entry));
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

    const pid_t parent = ::getpid();
    pid_t pid = 0;
    {
        // Under the lock, so kill_running_children() either runs before the
        // fork or sees the child, with its process group already set up.
        // The pipes are made and our copies of their write ends closed under
        // it too, so no other run()'s child inherits them (until its exec)
        // and keeps them open.
        std::lock_guard lock{g_children_mutex};
        make_pipe(out_r, out_w);
        make_pipe(err_r, err_w);
        make_pipe(rep_r, rep_w);
        pid = ::fork();
        if (pid == 0) {
            // The child has a copy of the locked mutex; it never touches it.
            child_exec(exe_str.c_str(), argv.data(), envp.data(), dev_null.get(), out_w.get(),
                       err_w.get(), rep_w.get(), parent);
        }
        if (pid < 0) {
            throw std::system_error(errno, std::generic_category(), "fork");
        }
        ::setpgid(pid, pid);  // also done in the child; whichever runs first wins
        track_child_locked(pid, true);
        out_w.reset();
        err_w.reset();
        rep_w.reset();
    }
    // From here on the child must never be left behind: if anything below
    // throws, the guard kills the whole group and reaps the child.
    ChildGuard guard{pid};
    dev_null.reset();

    // A pidfd becomes readable the moment the child exits (Linux >= 5.3).
    // Without one, the loop falls back to checking every kTick.
    const Fd pidfd{static_cast<int>(::syscall(SYS_pidfd_open, pid, 0))};

    const auto timeout = std::clamp(cmd.timeout, std::chrono::milliseconds{0}, kMaxWait);
    const auto kill_grace = std::clamp(cmd.kill_grace, std::chrono::milliseconds{0}, kMaxWait);
    // The deadline covers exec() too: a program on a hung mount, or a child
    // stopped before exec, is stopped like one that runs too long.
    auto deadline = start + timeout;
    bool term_sent = false, kill_sent = false;
    std::string report;  // the child's errno if exec() failed (sizeof(int) bytes)
    bool report_truncated = false;
    bool given_up = false;
    std::optional<Clock::time_point> drain_deadline;

    while (out_r.get() >= 0 || err_r.get() >= 0 || rep_r.get() >= 0 || !guard.exited()) {
        // Check the exit first, so a program that exited just before the
        // deadline is never reported as timed out.
        if (!guard.exited() && guard.try_exited()) {
            drain_deadline = Clock::now() + kDrainGrace;
        }
        if (guard.exited() && out_r.get() < 0 && err_r.get() < 0 && rep_r.get() < 0) {
            break;  // nothing left to wait for
        }
        const auto now = Clock::now();

        if (!guard.exited() && now >= deadline) {
            if (!term_sent) {
                guard.kill_group(SIGTERM);
                term_sent = true;
                deadline = now + kill_grace;
            } else if (!kill_sent) {
                guard.kill_group(SIGKILL);
                kill_sent = true;
                deadline = now + kKillWait;
            } else {
                // SIGKILL didn't end it: it is stuck in the kernel. Don't
                // hang with it; leave it to the kernel (and meerkat's exit).
                guard.abandon();
                given_up = true;
                break;
            }
        }
        if (guard.exited() && drain_deadline && now >= *drain_deadline) {
            // The program is gone but something it started still holds our
            // pipes. It is still a zombie, so its group id is still ours.
            guard.kill_group(SIGKILL);
            out_r.reset();
            err_r.reset();
            rep_r.reset();
            break;
        }

        // GCC's -Wextra wants every field named, so revents is spelled out.
        std::array<pollfd, 4> fds{{{.fd = out_r.get(), .events = POLLIN, .revents = 0},
                                   {.fd = err_r.get(), .events = POLLIN, .revents = 0},
                                   {.fd = rep_r.get(), .events = POLLIN, .revents = 0},
                                   {.fd = guard.exited() ? -1 : pidfd.get(), .events = POLLIN,
                                    .revents = 0}}};
        // Without a pidfd, wake up regularly to check for the exit. With one,
        // only the deadlines bound the wait.
        auto wait = (pidfd.get() >= 0 || guard.exited()) ? kMaxWait : kTick;
        if (!guard.exited()) {
            wait = std::min(wait, ceil_ms(deadline - now));
        }
        if (guard.exited() && drain_deadline) {
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
            if (fds[2].revents != 0) {
                drain(rep_r, report, sizeof(int), report_truncated);
            }
        }
    }
    guard.reap();

    int exec_errno = 0;
    if (report.size() == sizeof exec_errno) {  // one atomic write (< PIPE_BUF)
        std::memcpy(&exec_errno, report.data(), sizeof exec_errno);
    }
    const int status = guard.status();
    if (given_up) {
        r.outcome = ProcessResult::Outcome::timed_out;
        r.signal = SIGKILL;
        r.error = "still running after SIGKILL (stuck in the kernel, e.g. a hung mount); "
                  "left behind";
    } else if (exec_errno != 0) {
        r.outcome = ProcessResult::Outcome::spawn_failed;
        r.not_found = exec_errno == ENOENT;
        r.error = "cannot run " + exe_str + ": " + core::text::errno_text(exec_errno);
    } else if (term_sent) {
        // Stopped by us, however it ended (even with exit code 0 after SIGTERM).
        r.outcome = ProcessResult::Outcome::timed_out;
        r.signal = guard.status_known() && WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    } else if (!guard.status_known()) {
        // Someone else reaped the child (SIGCHLD ignored, or a waitpid(-1)
        // elsewhere): it is gone, but how it ended is unknown.
        r.outcome = ProcessResult::Outcome::exited;
        r.error = guard.wait_errno() == ECHILD
                      ? std::string{"exit status unavailable: the child was reaped elsewhere"}
                      : "exit status unavailable: waitpid: " +
                            core::text::errno_text(guard.wait_errno());
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
