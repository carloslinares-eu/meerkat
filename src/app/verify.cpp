/**
 * @file verify.cpp
 * @brief `meerkat verify`.
 */
#include "app/verify.hpp"

#include <exception>
#include <system_error>

#include "core/text.hpp"
#include "store/manifest.hpp"
#include "store/raw_log.hpp"

namespace meerkat::app {

namespace {

/// @return @p s padded with spaces to @p width.
std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) {
        s.append(width - s.size(), ' ');
    }
    return s;
}

}  // namespace

std::string verify_text(const store::VerifyResult& r, const std::filesystem::path& data_dir) {
    using core::text::plural;
    using core::text::visible;
    using O = store::EntryCheck::Outcome;
    std::string s = "Evidence trail in " + visible(data_dir.string()) + ", as of " +
                    store::utc_day(core::Timestamp{r.today}) + " (UTC)\n";
    std::size_t files = 0;
    for (const auto& m : r.manifests) {
        bool fine = m.manifest.has_value();
        for (const auto& e : m.entries) {
            fine = fine && e.outcome == O::ok;
        }
        for (const auto& f : r.findings) {  // e.g. a broken chain, or the wrong day inside
            fine = fine && !(f.level == store::VerifyFinding::Level::problem && f.day == m.day);
        }
        s += "\n" + m.day + "  " + (fine ? "ok" : "PROBLEM") +
             (m.sha256.empty() ? std::string{} : "  manifest sha256 " + m.sha256) + "\n";
        for (const auto& e : m.entries) {
            ++files;
            s += "    " + pad(outcome_name(e.outcome), 8) + " " + visible(e.entry.path) + " (" +
                 plural(e.entry.size, "byte") + ")";
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
        s += "OK. " + plural(r.manifests.size(), "manifest") + ", " +
             plural(files, "file") + " checked; nothing was changed after it was sealed.\n";
    } else {
        s += plural(problems, "problem") + " in " +
             plural(r.manifests.size(), "manifest") + " and " + plural(files, "file") +
             ".\n";
    }
    return s;
}

int verify_command(const std::filesystem::path& data_dir, core::Timestamp now, std::ostream& out,
                   std::ostream& err, const std::string& as_of) {
    auto today = std::chrono::floor<std::chrono::days>(now);
    if (!as_of.empty()) {
        const auto d = store::parse_day(as_of);
        if (!d) {
            err << "meerkat: --as-of must be a date like 2026-11-16 (got '" << core::text::visible(as_of) << "')\n";
            return 2;
        }
        today = *d;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec)) {
        err << "meerkat: no data directory " << core::text::visible(data_dir.string())
            << " (see --data-dir)\n";
        return 1;
    }
    try {
        const auto r = store::verify_data(data_dir, today);
        out << verify_text(r, data_dir);
        return r.ok() ? 0 : 1;
    } catch (const std::exception& e) {
        err << "meerkat: " << core::text::visible(e.what()) << '\n';
        return 1;
    }
}

}  // namespace meerkat::app
