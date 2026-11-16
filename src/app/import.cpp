/**
 * @file import.cpp
 * @brief Implementation of `meerkat import-bbm`.
 */
#include "app/import.hpp"

#include <exception>
#include <string>
#include <system_error>

#include "core/text.hpp"
#include "store/official.hpp"

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
    for (const auto& file : files) {
        const auto name = core::text::visible(file.string());
        try {
            const auto r = store::import_official(data_dir, file, to_utc);
            out << name << ": ";
            if (r.pdf) {
                out << (r.already_stored ? "already kept as " : "kept as ") << r.stored
                    << " (the protocol; not read)\n";
                continue;
            }
            out << r.measurements << (r.measurements == 1 ? " measurement" : " measurements") << ", "
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
