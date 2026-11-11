/**
 * @file text.hpp
 * @brief Internal helpers for reading procfs/sysfs files and parsing numbers.
 */
#pragma once

#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

/**
 * @namespace meerkat::net::detail
 * @brief Implementation helpers shared by the net parsers; not a public API.
 */
namespace meerkat::net::detail {

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
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
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

}  // namespace meerkat::net::detail
