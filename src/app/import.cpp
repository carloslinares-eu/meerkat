/**
 * @file import.cpp
 * @brief Implementation of `meerkat import-bbm`.
 */
#include "app/import.hpp"

#include <exception>
#include <optional>
#include <string>
#include <system_error>

#include "core/text.hpp"
#include "store/official.hpp"
#include "sys/privileges.hpp"

namespace meerkat::app {

int import_bbm_command(const std::filesystem::path& data_dir, const std::vector<std::filesystem::path>& files,
                       const probe::LocalToUtc& to_utc, std::ostream& out, std::ostream& err) {
    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec)) {
        err << "error: no data directory at " << core::text::visible(data_dir.string())
            << " (see --data-dir); the service's is /var/lib/meerkat\n";
        return 1;
    }
    int failed = 0;
    // Read every file first: as root they may be anywhere (#133).
    std::vector<std::optional<store::OfficialSource>> sources;
    for (const auto& file : files) {
        try {
            sources.emplace_back(store::read_official_source(file));
        } catch (const std::exception& e) {
            err << "error: " << core::text::visible(file.string()) << ": " << core::text::visible(e.what()) << "\n";
            sources.emplace_back();
            ++failed;
        }
    }
    // Then work in the data directory as its owner, never as root: root
    // would follow symlinks its owner put there (#133).
    try {
        if (const auto owner = sys::owner_to_become(data_dir)) {
            sys::become(*owner);
        }
    } catch (const std::exception& e) {
        err << "error: " << core::text::visible(e.what()) << "; nothing imported\n";
        return 1;
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!sources[i]) {
            continue;
        }
        const auto name = core::text::visible(files[i].string());
        try {
            const auto r = store::import_official(data_dir, *sources[i], to_utc);
            out << name << ": ";
            if (r.pdf) {
                out << (r.already_stored ? "already kept as " : "kept as ") << r.stored
                    << " (the protocol; not read)\n";
                continue;
            }
            out << core::text::plural(r.measurements, "measurement") << ", "
                << r.added << " new in " << store::kOfficialFile << "; "
                << (r.already_stored ? "the file was already kept as " : "the file is kept as ") << r.stored
                << "\n";
        } catch (const std::exception& e) {
            err << "error: " << name << ": " << core::text::visible(e.what()) << "\n";
            ++failed;
        }
    }
    if (failed == 0 && !files.empty()) {
        out << "The next daily manifest (just after midnight UTC, by `meerkat run`) seals them.\n";
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace meerkat::app
