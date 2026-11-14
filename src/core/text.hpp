/**
 * @file text.hpp
 * @brief Small text helpers shared by the parsers: file reading, trimming,
 *        line splitting and locale-independent number parsing.
 */
#pragma once

#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/**
 * @namespace meerkat::core::text
 * @brief Text helpers used by every parser. All number parsing goes through
 *        std::from_chars, so it never depends on the process locale.
 */
namespace meerkat::core::text {

/**
 * @brief Reads a whole file.
 * @param path The file.
 * @return Its contents (`""` for an empty file), or `std::nullopt` if it
 *         can't be opened or a read fails: a directory (EISDIR), an I/O
 *         error, a sysfs attribute that fails with EINVAL (e.g. `speed` on
 *         Wi-Fi), a /proc file of a process that just went away (ESRCH).
 */
inline std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    // istream::read, not `ss << in.rdbuf()`: when read() fails,
    // basic_filebuf::underflow throws; read() catches that and sets badbit
    // on `in`, while `<<` would set failbit on its target, the same as for
    // an empty file, and the error would look like an empty file.
    std::string text;
    char buf[4096];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) {
        text.append(buf, static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad()) {
        return std::nullopt;
    }
    return text;
}

/**
 * @brief Removes leading and trailing whitespace (spaces, tabs, CR, LF).
 * @param s The text.
 * @return The trimmed view.
 */
inline std::string_view trim(std::string_view s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/**
 * @brief Reads the first line of a sysfs attribute, without trailing whitespace.
 * @param path The attribute file.
 * @return The value, or `std::nullopt` if the file can't be read or is empty.
 */
inline std::optional<std::string> read_attr(const std::filesystem::path& path) {
    std::ifstream in{path};
    std::string line;
    if (!in || !std::getline(in, line)) {
        return std::nullopt;
    }
    return std::string{trim(line)};
}

/**
 * @brief Parses the whole of @p s as an integer; trailing junk is rejected.
 * @tparam T     Integer type.
 * @param s      The text.
 * @param base   10 or 16.
 * @return The number, or `std::nullopt` if @p s is empty, invalid or too big.
 */
template <typename T>
std::optional<T> parse_int(std::string_view s, int base = 10) {
    if (s.empty()) {
        return std::nullopt;
    }
    T value{};
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, value, base);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

/**
 * @brief Calls @p fn for every line of @p text (without the newline).
 *
 * A final line without a newline is included; an empty @p text gives no calls.
 *
 * @tparam Fn  Callable taking a `std::string_view`.
 * @param text The text.
 * @param fn   Called once per line.
 */
template <typename Fn>
void for_each_line(std::string_view text, Fn fn) {
    while (!text.empty()) {
        const auto nl = text.find('\n');
        fn(text.substr(0, nl));
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
    }
}

/**
 * @brief Parses the number at the start of @p s, ignoring what follows
 *        (e.g. `-58` in `-58 dBm`).
 *
 * `nan` and `inf` (which std::from_chars accepts, and tools print when they
 * have no figure, e.g. `rtt … = nan/nan/nan/nan`) aren't measurements: they
 * give no number, like any other text.
 * @param s The text, starting with the number.
 * @return The number, or `std::nullopt` if @p s doesn't start with a finite one.
 */
inline std::optional<double> leading_double(std::string_view s) {
    double v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr == s.data() || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

/**
 * @brief Parses the whole of @p s as a number; trailing junk is rejected.
 * @param s The text, e.g. `27.5`.
 * @return The number, or `std::nullopt` if @p s is empty, not entirely a
 *         number, or not finite (`nan`, `inf`; see leading_double()).
 */
inline std::optional<double> parse_double(std::string_view s) {
    double v = 0;
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (s.empty() || ec != std::errc{} || ptr != end || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

/**
 * @brief Formats a number for messages: the shortest text that reads back
 *        as the same number, always with `.` as the decimal point (e.g.
 *        `250`, `27.5`, `1000000`, `1234.5678`).
 *
 * Plain digits from 0.000001 to below 10^15. Beyond that, the shorter of
 * plain and exponent form (`1e+300`), so a typo in the config can't fill a
 * message with hundreds of digits. Zero is `0`, never `-0`.
 * @param v The number.
 * @return The text.
 */
inline std::string to_text(double v) {
    if (v == 0) {
        v = 0;  // -0 too
    }
    const double size = std::fabs(v);
    char buf[64];  // either form fits; the shortest form needs at most 24
    if (size >= 1e-6 && size < 1e15) {
        const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
        if (ec == std::errc{}) {
            return std::string(buf, ptr);
        }
    }
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, ec == std::errc{} ? ptr : buf);
}

/**
 * @brief `strerror` for an errno value, thread-safe (std::strerror isn't).
 * @param err The errno value.
 * @return Its text, e.g. `No such file or directory`.
 */
inline std::string errno_text(int err) {
    char buf[256] = {};
    // GNU strerror_r returns the text, which may or may not be in buf.
    return ::strerror_r(err, buf, sizeof buf);
}

/**
 * @brief Length of the valid UTF-8 character at the start of @p s.
 * @param s The text; not empty.
 * @return 1–4, or 0 if it doesn't start with a valid character (a stray
 *         continuation byte, a cut-off or overlong sequence, a surrogate,
 *         beyond U+10FFFF).
 */
inline std::size_t utf8_length(std::string_view s) {
    const auto b = [&s](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    const unsigned char c = b(0);
    std::size_t n = 0;
    unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the second byte
    if (c < 0x80) {
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        lo = c == 0xE0 ? 0xA0 : 0x80;  // no overlong forms
        hi = c == 0xED ? 0x9F : 0xBF;  // no surrogates
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        lo = c == 0xF0 ? 0x90 : 0x80;
        hi = c == 0xF4 ? 0x8F : 0xBF;  // at most U+10FFFF
    } else {
        return 0;
    }
    if (s.size() < n || b(1) < lo || b(1) > hi) {
        return 0;
    }
    for (std::size_t i = 2; i < n; ++i) {
        if ((b(i) & 0xC0U) != 0x80U) {
            return 0;
        }
    }
    return n;
}

/**
 * @brief Checks whether the valid UTF-8 character @p ch is a C1 control or
 *        an invisible formatting character (see visible()).
 * @param ch One whole character (utf8_length() bytes).
 * @return True if it has to be escaped.
 */
inline bool is_hidden(std::string_view ch) {
    const auto b = [&ch](std::size_t i) { return static_cast<unsigned char>(ch[i]); };
    if (ch.size() == 2) {
        return b(0) == 0xC2 && b(1) < 0xA0;  // U+0080–U+009F
    }
    if (ch.size() != 3) {
        return false;
    }
    const unsigned cp = ((b(0) & 0x0FU) << 12U) | ((b(1) & 0x3FU) << 6U) | (b(2) & 0x3FU);
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF;
}

/**
 * @brief Makes @p s safe to print: what can't be seen becomes visible.
 *
 * Control characters (below 0x20, 0x7f, and U+0080–U+009F), invisible
 * formatting characters that change how text reads (zero-width spaces and
 * joiners U+200B–U+200F, line and paragraph separators U+2028/U+2029, bidi
 * embeddings, overrides and isolates U+202A–U+202E and U+2066–U+2069, word
 * joiner and invisible operators U+2060–U+2064, U+FEFF) and bytes that
 * aren't valid UTF-8 become `\xNN`, one per byte, and `\` becomes `\\`,
 * so the result reads back unambiguously. Everything else (printable ASCII,
 * valid UTF-8 such as `ä` or `–`) stays as it is. For text from outside
 * (config values, file names, tool output) that goes into a message: a
 * newline can't split a log line, an escape sequence can't redraw the
 * terminal, and journald doesn't store the line as a blob. Only what is
 * printed changes; stored data keeps its bytes.
 * @param s The text.
 * @return The printable form.
 */
inline std::string visible(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size());
    const auto escape = [&out](unsigned char c) {
        out += "\\x";
        out += kHex[c >> 4U];
        out += kHex[c & 0xFU];
    };
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = utf8_length(s.substr(i));
        if (n == 0 || c < 0x20 || c == 0x7f || is_hidden(s.substr(i, n))) {
            const std::size_t bytes = n == 0 ? 1 : n;  // a whole hidden character
            for (std::size_t k = 0; k < bytes; ++k) {
                escape(static_cast<unsigned char>(s[i + k]));
            }
            i += bytes;
        } else if (c == '\\') {
            out += "\\\\";
            ++i;
        } else {
            out.append(s.substr(i, n));
            i += n;
        }
    }
    return out;
}

/**
 * @brief Joins @p parts with @p separator between them.
 * @param parts     The texts.
 * @param separator Put between two of them, e.g. `, `.
 * @return The joined text; empty for no parts.
 */
inline std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) {
            out += separator;
        }
        out += p;
    }
    return out;
}

/**
 * @brief Checks that @p name can stand for a host: 1–253 characters, no
 *        whitespace or control characters. A name given to a program as
 *        an argument needs is_host_argument(), which also refuses a leading `-`.
 * @param name Host name or address literal.
 * @return True if it is acceptable.
 */
inline bool is_host_text(std::string_view name) {
    if (name.empty() || name.size() > 253) {
        return false;
    }
    for (const char c : name) {
        if (static_cast<unsigned char>(c) <= ' ' || c == '\x7f') {
            return false;
        }
    }
    return true;
}

/**
 * @brief Checks that @p name can be given to a program as a host argument:
 *        is_host_text(), and not starting with `-`, which the program would
 *        read as an option.
 * @param name Host name or address literal.
 * @return True if it is acceptable.
 */
inline bool is_host_argument(std::string_view name) {
    return is_host_text(name) && name.front() != '-';
}

}  // namespace meerkat::core::text
