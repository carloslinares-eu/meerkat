/**
 * @file log.hpp
 * @brief Log lines for `meerkat run`: timestamped text, journald priorities,
 *        or a terminal with a live status line.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

#include "core/time.hpp"

namespace meerkat::app {

/// How important a log line is (syslog priorities, as journald uses them).
enum class LogLevel {
    error = 3,    ///< Something failed; data may be missing.
    warning = 4,  ///< The evidence is weakened (Wi-Fi, outage, no sleep lock).
    notice = 5,   ///< Normal but significant (start, stop, outage ended).
    info = 6,     ///< Routine (each speed test).
};

/// How lines are written.
enum class LogStyle {
    plain,     ///< `2026-01-01T00:00:00Z (01:00:00 CET) warning: text` (a file or a pipe).
    journal,   ///< `<4>text`: journald reads the priority and adds the time.
    terminal,  ///< Like plain, plus a status line kept at the bottom.
};

/**
 * @brief Picks the style for a file descriptor: `journal` if it is the
 *        stream systemd connected to the journal (`$JOURNAL_STREAM`),
 *        `terminal` if it is a TTY, `plain` otherwise.
 * @param fd             The descriptor, usually 2 (stderr).
 * @param journal_stream The value of `$JOURNAL_STREAM` (`<dev>:<inode>`), or empty.
 * @return The style.
 */
LogStyle detect_log_style(int fd, std::string_view journal_stream);

/**
 * @brief The width of the terminal on @p fd.
 * @param fd A descriptor.
 * @return Columns, or 0 if @p fd isn't a terminal.
 */
std::size_t terminal_columns(int fd);

/**
 * @brief Cuts UTF-8 text to at most @p max characters (code points), never
 *        in the middle of one.
 * @param text The text.
 * @param max  Most characters to keep.
 * @return The text, possibly shorter.
 */
std::string_view truncate_utf8(std::string_view text, std::size_t max);

/**
 * @brief A thread-safe line logger.
 *
 * Every line is written whole, so lines from the job threads never mix.
 * In the terminal style a status line (see status()) stays below the log
 * and is redrawn after each line.
 */
class Log {
public:
    /// Formats a timestamp as local clock time; the flag asks for the date
    /// too when it isn't the UTC date. Same contract as core::to_local_clock().
    using LocalClock = std::function<std::string(core::Timestamp, bool)>;

    /**
     * @brief Logs to @p out.
     * @param out   Where the lines go; must outlive the log.
     * @param style How they look.
     * @param now   Clock for the timestamps (tests pass a fixed one).
     * @param columns Terminal width for the status line, read before each
     *                redraw (the window can change); empty or 0: no limit.
     * @param local   Local clock time for a timestamp, see
     *                core::to_local_clock() (the default; tests pass a fixed
     *                one). Empty function or empty text: none shown.
     */
    Log(std::ostream& out, LogStyle style, std::function<core::Timestamp()> now = core::now,
        std::function<std::size_t()> columns = {}, LocalClock local = core::to_local_clock);

    /**
     * @brief Writes one line.
     * @param level How important.
     * @param text  The message, without a newline.
     */
    void write(LogLevel level, std::string_view text);

    /// @param text The message. @see write()
    void error(std::string_view text) { write(LogLevel::error, text); }
    /// @param text The message. @see write()
    void warning(std::string_view text) { write(LogLevel::warning, text); }
    /// @param text The message. @see write()
    void notice(std::string_view text) { write(LogLevel::notice, text); }
    /// @param text The message. @see write()
    void info(std::string_view text) { write(LogLevel::info, text); }

    /**
     * @brief Replaces the status line (terminal style only; ignored otherwise).
     *
     * It is cut to one character less than the terminal width, so it never
     * wraps (a wrapped line couldn't be erased).
     * @param text One line, without a newline.
     */
    void status(std::string_view text);

    /// Removes the status line, e.g. before exiting.
    void clear_status();

    /**
     * @brief A time of day for the status line: the local clock time with
     *        its zone (`07:49:00 CET`), or `HH:MM:SS` UTC if there's none.
     * @param t The time.
     * @return The text.
     */
    [[nodiscard]] std::string clock(core::Timestamp t) const;

    /// @return The style in use.
    [[nodiscard]] LogStyle style() const { return style_; }

private:
    /// Erases the status line from the terminal; the mutex must be held.
    void erase_status();

    std::ostream& out_;                         ///< Where lines go.
    LogStyle style_;                            ///< How they look.
    std::function<core::Timestamp()> now_;      ///< Timestamp source.
    std::function<std::size_t()> columns_;      ///< Terminal width, if known.
    LocalClock local_;                          ///< Local time text.
    std::mutex mutex_;                          ///< Keeps lines whole.
    std::string status_;                        ///< Current status line (terminal).
};

}  // namespace meerkat::app
