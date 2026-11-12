/**
 * @file text.hpp
 * @brief Small text helpers shared by the parsers: file reading, trimming,
 *        line splitting and locale-independent number parsing.
 */
#pragma once

#include <charconv>
#include <filesystem>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
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
 * @return Its contents, or `std::nullopt` if it can't be read (some sysfs
 *         attributes fail with EINVAL, e.g. `speed` on Wi-Fi).
 */
inline std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        return std::nullopt;
    }
    return ss.str();
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
 * @param s The text, starting with the number.
 * @return The number, or `std::nullopt` if @p s doesn't start with one.
 */
inline std::optional<double> leading_double(std::string_view s) {
    double v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr == s.data()) {
        return std::nullopt;
    }
    return v;
}

/**
 * @brief Parses the whole of @p s as a number; trailing junk is rejected.
 * @param s The text, e.g. `27.5`.
 * @return The number, or `std::nullopt` if @p s is empty or not entirely a number.
 */
inline std::optional<double> parse_double(std::string_view s) {
    double v = 0;
    const auto* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (s.empty() || ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return v;
}

/**
 * @brief Formats a number as briefly as possible, always with `.` as the
 *        decimal point (e.g. `250`, `27.5`), for messages.
 * @param v The number.
 * @return The text.
 */
inline std::string to_text(double v) {
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << v;
    return s.str();
}

}  // namespace meerkat::core::text
