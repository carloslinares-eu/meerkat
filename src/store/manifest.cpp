/**
 * @file manifest.cpp
 * @brief Implementation of the daily SHA-256 manifest.
 */
#include "store/manifest.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>

#include "core/text.hpp"
#include "store/append_file.hpp"
#include "store/sha256.hpp"

namespace meerkat::store {

namespace fs = std::filesystem;

namespace {

/// The manifest's first line.
constexpr std::string_view kMagic = "# meerkat manifest v1";
/// Extension of manifest files.
constexpr std::string_view kExt = ".sha256";

/**
 * @brief Whether @p p is a safe path relative to the data directory: not
 *        empty, not absolute, no `..`, no line breaks.
 * @param p The path text.
 * @return True if safe.
 */
bool plain_relative(std::string_view p) {
    if (p.empty() || p.find_first_of("\r\n") != std::string_view::npos) {
        return false;
    }
    const fs::path path{p};
    if (path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

/**
 * @brief Whether @p s is 64 lowercase hex digits.
 * @param s The text.
 * @return True if so.
 */
bool is_hex64(std::string_view s) {
    return s.size() == 64 &&
           s.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

/**
 * @brief Removes @p prefix from the front of @p s if it is there.
 * @param s      The text.
 * @param prefix The prefix.
 * @return True if it was there.
 */
bool consume(std::string_view& s, std::string_view prefix) {
    if (s.substr(0, prefix.size()) != prefix) {
        return false;
    }
    s.remove_prefix(prefix.size());
    return true;
}

/**
 * @brief The latest manifest of a day before @p day, as a path relative to
 *        the data directory.
 * @param data_dir The data directory.
 * @param day      `YYYY-MM-DD`.
 * @return `manifest/<earlier day>.sha256`, or empty if there is none.
 */
std::string previous_manifest(const fs::path& data_dir, std::string_view day) {
    const fs::path dir = data_dir / kManifestDir;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return {};
    }
    std::string best;
    for (const auto& e : fs::directory_iterator{dir, ec}) {
        const std::string name = e.path().filename().string();
        if (name.size() != 10 + kExt.size() || !name.ends_with(kExt)) {
            continue;
        }
        const std::string d = name.substr(0, 10);
        // ISO dates sort as text.
        if (valid_day(d) && d < day && d > best && e.is_regular_file(ec)) {
            best = d;
        }
    }
    return best.empty() ? std::string{} : std::string{kManifestDir} + "/" + best + std::string{kExt};
}

}  // namespace

bool valid_day(std::string_view day) {
    if (day.size() != 10) {
        return false;
    }
    // Round trip, so only the canonical form of a real date passes.
    const auto t = core::parse_iso8601(std::string{day} + "T00:00:00Z");
    return t && core::to_iso8601(*t).substr(0, 10) == day;
}

std::vector<std::string> daily_files(std::string_view day) {
    return {std::string{kRawDir} + "/" + std::string{day} + ".jsonl", std::string{kSpeedFile},
            std::string{kConnectivityFile}};
}

std::filesystem::path manifest_path(const std::filesystem::path& data_dir, std::string_view day) {
    return data_dir / kManifestDir / (std::string{day} + std::string{kExt});
}

Manifest build_manifest(const std::filesystem::path& data_dir, std::string_view day, core::Timestamp created,
                        const std::vector<std::string>& files) {
    if (!valid_day(day)) {
        throw std::invalid_argument("not a day (YYYY-MM-DD): " + std::string{day});
    }
    Manifest m;
    m.day = std::string{day};
    m.created = created;

    std::vector<std::string> paths;
    if (auto prev = previous_manifest(data_dir, day); !prev.empty()) {
        paths.push_back(std::move(prev));
    }
    for (const auto& f : files) {
        if (!plain_relative(f)) {
            throw std::invalid_argument("not a plain relative path: " + f);
        }
        paths.push_back(f);
    }
    for (const auto& p : paths) {
        const fs::path full = data_dir / p;
        std::error_code ec;
        if (!fs::is_regular_file(full, ec)) {
            continue;  // e.g. no speed test ran that day
        }
        // Size first, then hash exactly that much: the CSVs may grow meanwhile.
        const auto size = fs::file_size(full, ec);
        if (ec) {
            throw std::runtime_error("can't read the size of " + full.string() + ": " +
                                     ec.message());
        }
        auto hash = sha256_file(full, size);
        if (!hash) {
            throw std::runtime_error("can't hash " + full.string());
        }
        m.entries.push_back({std::move(*hash), size, p});
    }
    return m;
}

std::string format_manifest(const Manifest& m) {
    std::string out{kMagic};
    out += "\n# day " + m.day + "\n# created " + core::to_iso8601(m.created) + "\n";
    for (const auto& e : m.entries) {
        out += e.sha256 + "  " + std::to_string(e.size) + "  " + e.path + "\n";
    }
    return out;
}

std::optional<Manifest> parse_manifest(std::string_view text) {
    Manifest m;
    int line_no = 0;
    bool ok = true;
    bool ended = !text.empty() && text.back() == '\n';
    core::text::for_each_line(text, [&](std::string_view line) {
        if (!ok) {
            return;
        }
        ++line_no;
        if (line_no == 1) {
            ok = line == kMagic;
        } else if (line_no == 2) {
            ok = consume(line, "# day ") && valid_day(line);
            m.day = std::string{line};
        } else if (line_no == 3) {
            const auto t = consume(line, "# created ") ? core::parse_iso8601(line) : std::nullopt;
            ok = t.has_value();
            m.created = t.value_or(core::Timestamp{});
        } else {
            // <hash>  <size>  <path>
            const auto a = line.find("  ");
            const auto b = a == std::string_view::npos ? a : line.find("  ", a + 2);
            if (b == std::string_view::npos) {
                ok = false;
                return;
            }
            ManifestEntry e;
            e.sha256 = std::string{line.substr(0, a)};
            const auto size = core::text::parse_int<std::uint64_t>(line.substr(a + 2, b - a - 2));
            e.path = std::string{line.substr(b + 2)};
            ok = is_hex64(e.sha256) && size.has_value() && plain_relative(e.path);
            e.size = size.value_or(0);
            m.entries.push_back(std::move(e));
        }
    });
    if (!ok || line_no < 3 || !ended) {
        return std::nullopt;
    }
    return m;
}

std::filesystem::path write_manifest(const std::filesystem::path& data_dir, const Manifest& m) {
    if (!valid_day(m.day)) {
        throw std::invalid_argument("not a day (YYYY-MM-DD): " + m.day);
    }
    const fs::path dir = data_dir / kManifestDir;
    fs::create_directories(dir);
    const fs::path target = manifest_path(data_dir, m.day);
    if (fs::exists(target)) {
        throw std::system_error(std::make_error_code(std::errc::file_exists),
                                "manifest already exists: " + target.string());
    }
    const fs::path tmp = dir / ("." + m.day + std::string{kExt} + ".tmp");
    fs::remove(tmp);  // left over from a crash
    {
        AppendFile f{tmp};
        f.append(format_manifest(m));  // write + fdatasync
    }
    if (::chmod(tmp.c_str(), 0440) != 0) {
        const int err = errno;
        fs::remove(tmp);
        throw std::system_error(err, std::generic_category(), "can't chmod " + tmp.string());
    }
    // link() fails with EEXIST instead of replacing, unlike rename().
    if (::link(tmp.c_str(), target.c_str()) != 0) {
        const int err = errno;
        fs::remove(tmp);
        throw std::system_error(err, std::generic_category(),
                                "can't create " + target.string());
    }
    fs::remove(tmp);
    sync_parent_dir(target);
    return target;
}

std::vector<EntryCheck> verify_manifest(const std::filesystem::path& data_dir, const Manifest& m) {
    std::vector<EntryCheck> checks;
    checks.reserve(m.entries.size());
    for (const auto& e : m.entries) {
        EntryCheck c{e, EntryCheck::Outcome::missing};
        const fs::path full = data_dir / e.path;
        std::error_code ec;
        if (!plain_relative(e.path) || !fs::is_regular_file(full, ec)) {
            checks.push_back(std::move(c));
            continue;
        }
        const auto size = fs::file_size(full, ec);
        if (ec) {
            checks.push_back(std::move(c));
            continue;
        }
        if (size < e.size) {
            c.outcome = EntryCheck::Outcome::shorter;
        } else if (const auto hash = sha256_file(full, e.size); !hash) {
            c.outcome = EntryCheck::Outcome::missing;
        } else {
            c.outcome = *hash == e.sha256 ? EntryCheck::Outcome::ok : EntryCheck::Outcome::changed;
        }
        checks.push_back(std::move(c));
    }
    return checks;
}

}  // namespace meerkat::store
