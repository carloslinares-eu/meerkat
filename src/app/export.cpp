/**
 * @file export.cpp
 * @brief `meerkat export`.
 */
#include "app/export.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "app/verify.hpp"
#include "core/text.hpp"
#include "store/manifest.hpp"
#include "store/verify.hpp"
#include "store/zip.hpp"
#include "sys/atomic_file.hpp"
#include "sys/regular_file.hpp"

namespace meerkat::app {

namespace {

namespace fs = std::filesystem;

/// Exit code for a bad argument.
constexpr int kUsageError = 2;

/// One data file to put in the bundle.
struct DataFile {
    std::string path;        ///< Relative to the data directory, e.g. `raw/2026-11-11.jsonl`.
    std::uint64_t size = 0;  ///< Bytes to take.
    std::uint64_t cut = 0;   ///< Bytes at the end left out (a line still being written).
};

/**
 * @brief How much of a file that may still be written to is complete: its
 *        size, cut back to just after the last line break.
 * @param path The file.
 * @param size Its size.
 * @return The length to take; 0 if it has no line break.
 * @throws std::system_error if it can't be read.
 */
std::uint64_t complete_lines(const fs::path& path, std::uint64_t size) {
    const auto fd = sys::open_regular_file(path);
    std::vector<char> buf(1 << 16);
    std::uint64_t end = size;
    while (end > 0) {
        const auto n = std::min<std::uint64_t>(end, buf.size());
        const auto at = end - n;
        const auto got = ::pread(fd.get(), buf.data(), static_cast<std::size_t>(n), static_cast<off_t>(at));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            throw std::system_error(errno, std::generic_category(), "can't read " + path.string());
        }
        if (static_cast<std::uint64_t>(got) != n) {
            throw std::runtime_error(path.string() + " got shorter while it was being exported");
        }
        for (auto i = static_cast<std::size_t>(n); i > 0; --i) {
            if (buf[i - 1] == '\n') {
                return at + i;
            }
        }
        end = at;
    }
    return 0;
}

/// The data files of one moment, and the check of the trail at it.
struct Snapshot {
    std::vector<DataFile> files;  ///< Manifests first, then the CSVs and raw logs.
    store::VerifyResult check;    ///< store::verify_data() of the same state.
};

/**
 * @brief Takes a snapshot of the data files and checks it.
 *
 * The manifests are listed first and the data after them, so every
 * manifest in the bundle was written before the data was looked at and
 * finds what it sealed. The check runs right after; if a manifest was
 * sealed meanwhile (`meerkat run` near midnight UTC), all of it is taken
 * again, so verify.txt describes the files in the bundle.
 * @param data_dir The data directory.
 * @param today    Today (UTC), for the check.
 * @return The snapshot.
 * @throws std::exception if the data can't be listed or read.
 */
Snapshot snapshot(const fs::path& data_dir, std::chrono::sys_days today) {
    Snapshot snap;
    for (int attempt = 0; attempt < 3; ++attempt) {
        snap = {};
        const auto manifests = store::manifest_days(data_dir);
        for (const auto& day : manifests) {
            auto p = store::manifest_path({}, day).string();
            const auto size = fs::file_size(data_dir / p);
            snap.files.push_back({std::move(p), size, 0});  // written once: whole
        }
        std::vector<std::string> growing;
        for (const auto f : {store::kSpeedFile, store::kConnectivityFile}) {
            if (fs::symlink_status(data_dir / f).type() == fs::file_type::regular) {
                growing.emplace_back(f);
            }
        }
        for (const auto& day : store::raw_log_days(data_dir)) {
            growing.push_back(std::string{store::kRawDir} + "/" + day + ".jsonl");
        }
        for (auto& p : growing) {
            const auto full = data_dir / p;
            const auto size = fs::file_size(full);
            const auto take = complete_lines(full, size);
            snap.files.push_back({std::move(p), take, size - take});
        }
        snap.check = store::verify_data(data_dir, today);
        std::vector<std::string> checked;
        for (const auto& m : snap.check.manifests) {
            checked.push_back(m.day);
        }
        if (checked == manifests) {
            break;
        }
    }
    return snap;
}

/// @return The README of the bundle.
std::string readme(const ReportRange& range, const ReportEnv& env, core::Timestamp now, bool verified) {
    return "meerkat export: " + day_text(range.from) + " to " + day_text(range.to) + " (" + env.zone.name +
           ")\n"
           "Made " + core::to_iso8601(now) + " by meerkat " + env.version + ".\n"
           "\n"
           "meerkat measures an internet connection around the clock: a speed test\n"
           "every few minutes, connectivity probes every few seconds. This bundle has\n"
           "everything it recorded, and the report made from it.\n"
           "\n"
           "What's in here\n"
           "  report.html       The report for the days above. Opens in any browser,\n"
           "                    prints to PDF.\n"
           "  verify.txt        The check of the evidence trail, made just before this\n"
           "                    bundle (`meerkat verify`). Result: " +
           std::string{verified ? "OK" : "PROBLEMS, see the file"} +
           ".\n"
           "  data/             meerkat's data directory as recorded, byte for byte:\n"
           "    speed.csv         every speed test, one row each\n"
           "    connectivity.csv  every connectivity round (ping, DNS)\n"
           "    raw/              the raw output of every test, one file per UTC day\n"
           "    manifest/         one SHA-256 manifest per UTC day, each including\n"
           "                      the manifest of the day before\n"
           "  SHA256SUMS        SHA-256 of every file above.\n"
           "\n"
           "The data covers the whole recording, not only the report's days: the\n"
           "manifests seal the files as a whole. Files that were still being written\n"
           "end at their last complete line.\n"
           "\n"
           "How to check it\n"
           "  sha256sum -c SHA256SUMS\n"
           "      In this folder: the files are as they were exported.\n"
           "  meerkat verify --data-dir data\n"
           "      Hashes every file each manifest sealed again and checks the chain.\n"
           "  Without meerkat: each line of data/manifest/<day>.sha256 is\n"
           "  \"<sha256>  <size>  <path>\", and\n"
           "      head -c <size> data/<path> | sha256sum\n"
           "  prints the same hash if those bytes weren't changed since that day.\n";
}

}  // namespace

int export_command(const core::Config& config, const ReportOptions& options, const ReportEnv& clock_env,
                   std::ostream& out, std::ostream& err) {
    const auto env = fixed_clock(clock_env);  // one moment for the range, the page and the archive (#89)
    const auto range = report_range(config, options, env, err);
    if (!range) {
        return kUsageError;
    }
    const std::string folder = "meerkat-export-" + day_text(range->from) + "-to-" + day_text(range->to);
    const auto output = options.output.empty() ? fs::path{folder + ".zip"} : options.output;
    if (inside_data_dir(output, config.data_dir)) {
        err << "meerkat: won't write the export into the data directory "
            << core::text::visible(config.data_dir.string())
            << " (it holds the evidence; choose another place with -o)\n";
        return kUsageError;
    }
    std::error_code ec;
    if (!fs::is_directory(config.data_dir, ec)) {
        err << "meerkat: no data directory " << core::text::visible(config.data_dir.string())
            << " (see --data-dir)\n";
        return 1;
    }
    try {
        const auto now = env.now();
        const auto snap = snapshot(config.data_dir, std::chrono::floor<std::chrono::days>(now));
        const auto& check = snap.check;
        const auto report = build_report(config, *range, env);

        // 0600: the bundle has the contract figures and the line's data in it.
        sys::AtomicFile file{output};
        store::ZipWriter zip{[&file](std::string_view b) { file.write(b); }, now};
        std::string sums;
        const auto add = [&](const std::string& name, const std::string& text) {
            sums += zip.add(folder + "/" + name, text) + "  " + name + "\n";
        };
        add("README.txt", readme(*range, env, now, check.ok()));
        add("report.html", report.html);
        add("verify.txt", verify_text(check, "data"));
        for (const auto& f : snap.files) {
            // The hash of the very bytes archived (#89).
            sums += zip.add_file(folder + "/data/" + f.path, config.data_dir / f.path, f.size) + "  data/" +
                    f.path + "\n";
        }
        zip.add(folder + "/SHA256SUMS", sums);
        zip.finish();
        file.commit();

        out << "Wrote " << core::text::visible(output.string()) << " (report " << day_text(range->from) << " to "
            << day_text(range->to) << ", " << core::text::plural(snap.files.size(), "data file") << ").\n"
            << report.summary;
        for (const auto& f : snap.files) {
            if (f.cut > 0) {
                out << "Left out the last " << core::text::plural(f.cut, "byte") << " of "
                    << core::text::visible(f.path) << ": a line still being written.\n";
            }
        }
        if (!report.speed_file) {
            out << "No speed.csv in " << core::text::visible(config.data_dir.string()) << " (see --data-dir).\n";
        }
        if (check.ok()) {
            out << "Evidence trail: OK (verify.txt in the bundle).\n";
        } else {
            out << "Evidence trail: " << core::text::plural(check.problems(), "problem")
                << "; see verify.txt in the bundle, or run `meerkat verify`.\n";
        }
    } catch (const std::exception& e) {
        err << "meerkat: " << core::text::visible(e.what()) << '\n';
        return 1;
    }
    return 0;
}

}  // namespace meerkat::app
