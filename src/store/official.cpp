/**
 * @file official.cpp
 * @brief Implementation of the official measurements' storage.
 */
#include "store/official.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <set>
#include <stdexcept>
#include <system_error>

#include "core/text.hpp"
#include "store/csv_reader.hpp"
#include "store/dir_lock.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"
#include "sys/atomic_file.hpp"
#include "sys/regular_file.hpp"

namespace meerkat::store {

namespace fs = std::filesystem;

namespace {

/// @return `yes`, `no` or empty.
std::string yes_no(std::optional<bool> v) {
    if (!v) {
        return {};
    }
    return *v ? "yes" : "no";
}

/**
 * @brief When run as root, gives @p path the data directory's owner and
 *        group, so `meerkat run` (the service's own user) can go on.
 * @param path     A file or folder just created.
 * @param data_dir The data directory.
 * @throws std::system_error if it can't be changed.
 */
void match_owner(const fs::path& path, const fs::path& data_dir) {
    if (::geteuid() != 0) {
        return;
    }
    struct stat dir {};
    if (::stat(data_dir.c_str(), &dir) != 0 || ::lchown(path.c_str(), dir.st_uid, dir.st_gid) != 0) {
        throw std::system_error(errno, std::generic_category(), "can't set the owner of " + path.string());
    }
}

/// @return The Test-IDs already in `official.csv`.
std::set<std::string> known_test_ids(const fs::path& file) {
    std::set<std::string> ids;
    std::size_t col = 0;
    const auto& columns = official_schema().columns;
    while (col < columns.size() && columns[col] != "test_id") {
        ++col;
    }
    try {
        read_csv(file, official_schema(), [&](const std::vector<std::string>& f) {
            ids.insert(f[col]);
            return true;
        });
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
    }
    return ids;
}

}  // namespace

const Schema& official_schema() {
    static const Schema schema{
        .name = "official",
        .version = 1,
        .columns = {"time", "source", "number", "download_mbps", "upload_mbps", "latency_ms",
                    "contract_down_min", "contract_down_normal", "contract_down_max", "contract_up_min",
                    "contract_up_normal", "contract_up_max", "connection", "on_power", "nic_sufficient",
                    "data_rate_limited", "test_id", "app_version", "file"}};
    return schema;
}

std::vector<std::string> official_row(const probe::OfficialMeasurement& m, const std::string& file) {
    return {core::to_iso8601(m.time),
            "breitbandmessung",
            m.number ? std::to_string(*m.number) : std::string{},
            format_number(m.download_mbps, 2),
            format_number(m.upload_mbps, 2),
            format_number(m.latency_ms, 1),
            format_number(m.contract_down_min, 2),
            format_number(m.contract_down_normal, 2),
            format_number(m.contract_down_max, 2),
            format_number(m.contract_up_min, 2),
            format_number(m.contract_up_normal, 2),
            format_number(m.contract_up_max, 2),
            m.connection,
            yes_no(m.on_power),
            yes_no(m.nic_sufficient),
            yes_no(m.data_rate_limited),
            m.test_id,
            m.app_version,
            file};
}

OfficialImport import_official(const std::filesystem::path& data_dir, const std::filesystem::path& source,
                               const probe::LocalToUtc& to_utc) {
    const auto content = sys::read_regular_file(source, kMaxOfficialFile + 1);
    if (content.size() > kMaxOfficialFile) {
        throw std::runtime_error(source.string() + " is bigger than an export can be; not imported");
    }
    const auto name = source.filename().string();
    if (!importable_name(name)) {
        throw std::runtime_error("can't import a file named '" + core::text::visible(name) +
                                 "': rename it (no leading '.', no control characters)");
    }
    OfficialImport r;
    r.stored = std::string{kOfficialDir} + "/" + name;
    r.pdf = content.starts_with("%PDF-");
    // The app names itself in the PDF's Creator ("… Festnetz-Check/3.13.0 …").
    if (r.pdf && content.find("Festnetz-Check") == std::string::npos) {
        throw std::runtime_error("a PDF, but not one the Breitbandmessung app (Festnetz-Check) made; not imported");
    }
    std::vector<probe::OfficialMeasurement> measurements;
    if (!r.pdf) {
        measurements = probe::parse_bbm_csv(content, to_utc);  // refuse before writing anything
        r.measurements = measurements.size();
    }

    // The original, byte for byte.
    const auto dir = data_dir / kOfficialDir;
    std::error_code ec;
    fs::create_directory(dir, ec);
    if (ec) {
        throw std::system_error(ec, "can't create " + dir.string());
    }
    // Every time, not only when created: a run that failed half-way left it as it was.
    fs::permissions(dir, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    match_owner(dir, data_dir);
    // One import at a time, so two can't both add the same Test-IDs.
    const DirectoryLock lock{dir, std::chrono::seconds{10}};
    if (lock.state() == DirectoryLock::State::busy) {
        throw std::runtime_error("another import-bbm is running; try again when it's done");
    }
    const auto target = data_dir / r.stored;
    try {
        r.already_stored = sys::read_regular_file(target, kMaxOfficialFile + 1) == content;
        if (!r.already_stored) {
            throw std::runtime_error(r.stored + " already holds a different file; rename the export to import it");
        }
    } catch (const std::system_error& e) {
        if (e.code() != std::errc::no_such_file_or_directory) {
            throw;
        }
        sys::AtomicFile copy{target};
        copy.write(content);
        copy.commit();
        fs::permissions(target, fs::perms::owner_read | fs::perms::group_read);
        match_owner(target, data_dir);
    }

    // The measurements not in official.csv yet.
    if (!measurements.empty()) {
        const auto csv = data_dir / kOfficialFile;
        auto known = known_test_ids(csv);
        const bool created = !fs::exists(csv);
        CsvWriter writer{csv, official_schema()};
        if (created) {
            match_owner(csv, data_dir);
        }
        for (const auto& m : measurements) {
            if (known.insert(m.test_id).second) {  // once, even if the export repeats it
                writer.write_row(official_row(m, r.stored));
                ++r.added;
            }
        }
    }
    return r;
}

}  // namespace meerkat::store
