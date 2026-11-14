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
