/**
 * @file time.cpp
 * @brief Implementation of the UTC timestamp helpers.
 */
#include "core/time.hpp"

#include <array>
#include <cstdio>

namespace meerkat::core {

namespace {

/**
 * @brief Reads exactly @p n decimal digits from the front of @p s.
 * @param s     The text; on success the digits are removed from the front.
 * @param n     Number of digits to read.
 * @param value Receives the number.
 * @return True if @p n digits were present.
 */
bool take_digits(std::string_view& s, std::size_t n, int& value) {
    if (s.size() < n) {
        return false;
    }
    int v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    value = v;
    s.remove_prefix(n);
    return true;
}

/**
 * @brief Removes @p c from the front of @p s if it is there.
 * @param s The text.
 * @param c The expected character.
 * @return True if @p c was removed.
 */
bool take_char(std::string_view& s, char c) {
    if (s.empty() || s.front() != c) {
        return false;
    }
    s.remove_prefix(1);
    return true;
}

}  // namespace

Timestamp now() {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

std::string to_iso8601(Timestamp t) {
    using namespace std::chrono;
    const auto day = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss hms{t - day};
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%04d-%02u-%02uT%02d:%02d:%02dZ", int{ymd.year()},
                  unsigned{ymd.month()}, unsigned{ymd.day()}, static_cast<int>(hms.hours().count()),
                  static_cast<int>(hms.minutes().count()),
                  static_cast<int>(hms.seconds().count()));
    return std::string{buf.data()};
}

std::optional<Timestamp> parse_iso8601(std::string_view text) {
    using namespace std::chrono;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (!take_digits(text, 4, y) || !take_char(text, '-') || !take_digits(text, 2, mo) ||
        !take_char(text, '-') || !take_digits(text, 2, d)) {
        return std::nullopt;
    }
    if (!take_char(text, 'T') && !take_char(text, 't') && !take_char(text, ' ')) {
        return std::nullopt;
    }
    if (!take_digits(text, 2, h) || !take_char(text, ':') || !take_digits(text, 2, mi) ||
        !take_char(text, ':') || !take_digits(text, 2, sec)) {
        return std::nullopt;
    }
    if (take_char(text, '.')) {
        std::size_t n = 0;
        while (n < text.size() && text[n] >= '0' && text[n] <= '9') {
            ++n;
        }
        if (n == 0) {
            return std::nullopt;
        }
        text.remove_prefix(n);
    }

    int offset_minutes = 0;
    if (take_char(text, 'Z') || take_char(text, 'z')) {
        // UTC
    } else if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        const int sign = text.front() == '-' ? -1 : 1;
        text.remove_prefix(1);
        int oh = 0, om = 0;
        if (!take_digits(text, 2, oh)) {
            return std::nullopt;
        }
        if (take_char(text, ':')) {
            if (!take_digits(text, 2, om)) {
                return std::nullopt;
            }
        } else if (!text.empty() && !take_digits(text, 2, om)) {
            return std::nullopt;
        }
        if (oh > 23 || om > 59) {
            return std::nullopt;
        }
        offset_minutes = sign * (oh * 60 + om);
    } else {
        return std::nullopt;  // no zone: ambiguous local time
    }
    if (!text.empty()) {
        return std::nullopt;
    }

    const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)},
                             day{static_cast<unsigned>(d)}};
    // A leap second (:60) only exists at 23:59 UTC; it is folded into the next
    // second. Anywhere else :60 is a malformed time.
    const int utc_minute_of_day = ((h * 60 + mi - offset_minutes) % 1440 + 1440) % 1440;
    const bool leap_second = sec == 60 && utc_minute_of_day == 23 * 60 + 59;
    if (!ymd.ok() || h > 23 || mi > 59 || (sec > 59 && !leap_second)) {
        return std::nullopt;
    }
    const Timestamp local{sys_days{ymd} + hours{h} + minutes{mi} + seconds{sec}};
    return local - minutes{offset_minutes};
}

}  // namespace meerkat::core
