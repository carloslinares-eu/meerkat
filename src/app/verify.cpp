/**
 * @file verify.cpp
 * @brief `meerkat verify`.
 */
#include "app/verify.hpp"

#include <exception>
#include <system_error>

#include "core/text.hpp"

namespace meerkat::app {

namespace {

/// @return @p s padded with spaces to @p width.
std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) {
        s.append(width - s.size(), ' ');
    }
    return s;
}

/// @return "1 thing" or "N things".
std::string count(std::size_t n, const std::string& one, const std::string& many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

std::string verify_text(const store::VerifyResult& r, const std::filesystem::path& data_dir) {
    using core::text::visible;
    using O = store::EntryCheck::Outcome;
    std::string s = "Evidence trail in " + visible(data_dir.string()) + "\n";
    std::size_t files = 0;
    for (const auto& m : r.manifests) {
        bool fine = m.manifest.has_value();
        for (const auto& e : m.entries) {
            fine = fine && e.outcome == O::ok;
        }
        s += "\n" + m.day + "  " + (fine ? "ok" : "PROBLEM") +
             (m.sha256.empty() ? std::string{} : "  manifest sha256 " + m.sha256) + "\n";
        for (const auto& e : m.entries) {
            ++files;
            s += "    " + pad(outcome_name(e.outcome), 8) + " " + visible(e.entry.path) + " (" +
                 count(e.entry.size, "byte", "bytes") + ")";
            if (e.outcome != O::ok) {
                s += ": " + store::outcome_meaning(e.outcome);
            }
            s += "\n";
        }
    }
    if (r.manifests.empty()) {
        s += "\nNo manifests.\n";
    }
    for (const auto level : {store::VerifyFinding::Level::problem, store::VerifyFinding::Level::note}) {
        bool header = false;
        for (const auto& f : r.findings) {
            if (f.level != level) {
                continue;
            }
            if (!header) {
                s += level == store::VerifyFinding::Level::problem ? "\nProblems:\n" : "\nNotes:\n";
                header = true;
            }
            s += "  - " + visible(f.text) + "\n";
        }
    }
    const auto problems = r.problems();
    s += "\nResult: ";
    if (problems == 0) {
        s += "OK. " + count(r.manifests.size(), "manifest", "manifests") + ", " +
             count(files, "file", "files") + " checked; nothing was changed after it was sealed.\n";
    } else {
        s += count(problems, "problem", "problems") + " in " +
             count(r.manifests.size(), "manifest", "manifests") + " and " + count(files, "file", "files") +
             ".\n";
    }
    return s;
}

int verify_command(const std::filesystem::path& data_dir, core::Timestamp now, std::ostream& out,
                   std::ostream& err) {
    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec)) {
        err << "meerkat: no data directory " << core::text::visible(data_dir.string())
            << " (see --data-dir)\n";
        return 1;
    }
    try {
        const auto r = store::verify_data(data_dir, std::chrono::floor<std::chrono::days>(now));
        out << verify_text(r, data_dir);
        return r.ok() ? 0 : 1;
    } catch (const std::exception& e) {
        err << "meerkat: " << core::text::visible(e.what()) << '\n';
        return 1;
    }
}

}  // namespace meerkat::app
