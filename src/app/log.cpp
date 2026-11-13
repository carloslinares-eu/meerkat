/**
 * @file log.cpp
 * @brief Implementation of the run logger.
 */
#include "app/log.hpp"

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <charconv>
#include <utility>

namespace meerkat::app {

namespace {

/**
 * @brief The word for a level in plain lines.
 * @param level The level.
 * @return E.g. `warning`.
 */
std::string_view level_word(LogLevel level) {
    switch (level) {
    case LogLevel::error: return "error";
    case LogLevel::warning: return "warning";
    case LogLevel::notice: return "notice";
    case LogLevel::info: break;
    }
    return "info";
}

/**
 * @brief Escapes control bytes so a message stays on one physical line.
 * @param text The untrusted or multi-line message.
 * @return A single-line representation with control bytes escaped.
 */
std::string single_line(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string line;
    line.reserve(text.size());
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '\n': line += "\\n"; break;
        case '\r': line += "\\r"; break;
        case '\t': line += "\\t"; break;
        default:
            if (byte < 0x20U || byte == 0x7FU) {
                line += "\\x";
                line += hex[byte >> 4U];
                line += hex[byte & 0x0FU];
            } else {
                line.push_back(raw);
            }
        }
    }
    return line;
}

/**
 * @brief Parses an unsigned decimal number.
 * @param text The text.
 * @param out  The value.
 * @return True if all of @p text was a number.
 */
bool parse_unsigned(std::string_view text, unsigned long long& out) {
    const auto* end = text.data() + text.size();
    const auto r = std::from_chars(text.data(), end, out);
    return !text.empty() && r.ec == std::errc{} && r.ptr == end;
}

}  // namespace

LogStyle detect_log_style(int fd, std::string_view journal_stream) {
    // systemd sets JOURNAL_STREAM to the device and inode of the stream it
    // connected; it only counts if that is still what fd points to.
    if (const auto colon = journal_stream.find(':'); colon != std::string_view::npos) {
        unsigned long long dev = 0;
        unsigned long long ino = 0;
        struct stat st {};
        if (parse_unsigned(journal_stream.substr(0, colon), dev) &&
            parse_unsigned(journal_stream.substr(colon + 1), ino) && ::fstat(fd, &st) == 0 &&
            static_cast<unsigned long long>(st.st_dev) == dev &&
            static_cast<unsigned long long>(st.st_ino) == ino) {
            return LogStyle::journal;
        }
    }
    return ::isatty(fd) == 1 ? LogStyle::terminal : LogStyle::plain;
}

std::size_t terminal_columns(int fd) {
    winsize ws{};
    if (::ioctl(fd, TIOCGWINSZ, &ws) != 0) {
        return 0;
    }
    return ws.ws_col;
}

std::string_view truncate_utf8(std::string_view text, std::size_t max) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        // A byte that doesn't continue a character starts a new one.
        if ((static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U && chars++ == max) {
            return text.substr(0, i);
        }
    }
    return text;
}

Log::Log(std::ostream& out, LogStyle style, std::function<core::Timestamp()> now,
         std::function<std::size_t()> columns)
    : out_{out}, style_{style}, now_{std::move(now)}, columns_{std::move(columns)} {}

void Log::erase_status() {
    if (style_ == LogStyle::terminal && !status_.empty()) {
        out_ << "\r\033[K";
    }
}

void Log::write(LogLevel level, std::string_view text) {
    std::lock_guard lock{mutex_};
    const auto line = single_line(text);
    if (style_ == LogStyle::journal) {
        out_ << '<' << static_cast<int>(level) << '>' << line << '\n' << std::flush;
        return;
    }
    erase_status();
    out_ << core::to_iso8601(now_()) << ' ' << level_word(level) << ": " << line << '\n';
    if (style_ == LogStyle::terminal && !status_.empty()) {
        out_ << status_;
    }
    out_ << std::flush;
}

void Log::status(std::string_view text) {
    if (style_ != LogStyle::terminal) {
        return;
    }
    std::lock_guard lock{mutex_};
    erase_status();
    const std::size_t width = columns_ ? columns_() : 0;
    const auto line = single_line(text);
    status_ = width > 1 ? truncate_utf8(line, width - 1) : line;
    out_ << status_ << std::flush;
}

void Log::clear_status() {
    std::lock_guard lock{mutex_};
    erase_status();
    status_.clear();
    out_ << std::flush;
}

}  // namespace meerkat::app
