/**
 * @file report.cpp
 * @brief `meerkat report`.
 */
#include "app/report.hpp"

#include <algorithm>
#include <exception>

#include "app/recorder.hpp"
#include "core/outage.hpp"
#include "core/text.hpp"
#include "report/advice.hpp"
#include "report/data.hpp"
#include "report/format.hpp"
#include "report/html.hpp"
#include "store/csv_reader.hpp"
#include "store/manifest.hpp"
#include "store/schema.hpp"
#include "store/verify.hpp"
#include "sys/atomic_file.hpp"

namespace meerkat::app {

namespace {

using std::chrono::days;
using std::chrono::seconds;
using std::chrono::sys_days;

/// Exit code for a bad argument.
constexpr int kUsageError = 2;
/// Days covered when there's no data to start from.
constexpr days kDefaultSpan{14};
/// The longest range one report covers: a year, and a day for a leap year.
constexpr days kMaxSpan{366};

/**
 * @brief The time of the first row of speed.csv.
 * @param data_dir The data directory.
 * @return The time, or nothing without a readable first row.
 */
std::optional<core::Timestamp> first_test(const std::filesystem::path& data_dir) {
    std::optional<core::Timestamp> first;
    try {
        store::read_csv(data_dir / store::kSpeedFile, store::speed_schema(),
                        [&first](const std::vector<std::string>& row) {
                            first = row.empty() ? std::nullopt : core::parse_iso8601(row[0]);
                            return !first;  // stop at the first readable time
                        });
    } catch (const std::exception&) {
        return std::nullopt;  // load_speed() reports it properly
    }
    return first;
}

/**
 * @brief The manifests of the UTC days @p period touches.
 * @param data_dir The data directory.
 * @param period   The period.
 * @return One entry per day, in order.
 */
std::vector<report::ManifestInfo> manifests(const std::filesystem::path& data_dir,
                                            const report::Period& period) {
    std::vector<report::ManifestInfo> out;
    const auto first = std::chrono::floor<days>(period.from);
    const auto last = std::chrono::floor<days>(period.to - seconds{1});
    for (auto d = first; d <= last; d += days{1}) {
        report::ManifestInfo info;
        info.day = day_text(d);
        // Read as `meerkat verify` reads it, so both describe it alike.
        auto file = store::read_manifest_file(data_dir, info.day);
        info.sha256 = std::move(file.sha256);
        info.manifest = std::move(file.manifest);
        if (file.missing) {
            info.problem = "no manifest (the day isn't sealed yet, or meerkat wasn't running)";
        } else if (!file.problem.empty()) {
            info.problem = "the manifest " + file.problem;
        }
        out.push_back(std::move(info));
    }
    return out;
}

/// @return One line about a direction's verdict.
std::string verdict_line(std::string_view name, const report::DirectionReport& d) {
    std::string s = std::string{name} + ": " + std::to_string(d.speeds.count) + " cable tests";
    if (!d.thresholds) {
        return s + ", no contract figures";
    }
    if (d.windows.empty()) {
        return s + ", too few measurement days for a three-day window";
    }
    return s + ", deviation in " + std::to_string(d.deviating_windows()) + " of " +
           std::to_string(d.windows.size()) + " overlapping three-day windows (" +
           std::to_string(d.separate_deviations()) + " without overlap)";
}

/**
 * @brief The official measurements in one or two lines.
 * @param o The analysis.
 * @return Empty without any; else e.g. `Official: 30 measurements, 1
 *         complete campaign: download deviation, upload none.` and a newline.
 */
std::string official_line(const report::OfficialAnalysis& o) {
    if (!o.any()) {
        return {};
    }
    std::string s = "Official: " + core::text::plural(o.rows.size(), "measurement") + ", " +
                    core::text::plural(o.campaigns.size(), "complete campaign");
    const auto one = [](const report::OfficialDirection& d) {
        return !d.contract ? std::string{"no contract figures"} : d.verdict.deviation() ? "deviation" : "none";
    };
    for (const auto& c : o.campaigns) {
        s += "; " + report::date(c.days[0]) + ": download " + one(c.download) + ", upload " + one(c.upload);
    }
    return s + ".\n";
}

}  // namespace

Zone utc_zone() {
    return {"UTC", report::utc_local(), [](sys_days d) { return core::Timestamp{d}; }};
}

Zone system_zone() {
    const std::chrono::time_zone* zone = nullptr;
    try {
        zone = std::chrono::current_zone();
    } catch (const std::exception&) {
        return utc_zone();
    }
    return {std::string{zone->name()}, report::system_local(), [zone](sys_days d) {
                const std::chrono::local_seconds local{std::chrono::local_days{d.time_since_epoch()}};
                const auto info = zone->get_info(local);
                if (info.result == std::chrono::local_info::nonexistent) {
                    return std::chrono::floor<seconds>(info.second.begin);  // the day starts after the gap
                }
                return core::Timestamp{local.time_since_epoch() - info.first.offset};
            }};
}

std::string day_text(std::chrono::sys_days d) { return core::to_iso_day(d); }

bool inside_data_dir(const std::filesystem::path& output, const std::filesystem::path& data_dir) {
    std::error_code ec;
    const auto dir = std::filesystem::weakly_canonical(std::filesystem::absolute(output, ec).parent_path(), ec);
    const auto data = std::filesystem::weakly_canonical(data_dir, ec);
    if (data.empty()) {
        return false;
    }
    const auto [d, o] = std::mismatch(data.begin(), data.end(), dir.begin(), dir.end());
    return d == data.end();
}

std::optional<ReportRange> report_range(const core::Config& config, const ReportOptions& options,
                                        const ReportEnv& env, std::ostream& err) {
    const auto& zone = env.zone;
    std::optional<sys_days> to = zone.local(env.now()).day;
    if (!options.to.empty()) {
        to = store::parse_day(options.to);
        if (!to) {
            err << "meerkat: --to must be a date like 2026-03-31 (got '" << core::text::visible(options.to) << "')\n";
            return std::nullopt;
        }
    }
    std::optional<sys_days> from;
    if (!options.from.empty()) {
        from = store::parse_day(options.from);
        if (!from) {
            err << "meerkat: --from must be a date like 2026-03-01 (got '" << core::text::visible(options.from)
                << "')\n";
            return std::nullopt;
        }
    } else if (const auto first = first_test(config.data_dir)) {
        // A first row from far back (a clock that was wrong) doesn't make
        // a report of decades.
        from = std::clamp(zone.local(*first).day, *to - (kMaxSpan - days{1}), *to);
    } else {
        from = *to - (kDefaultSpan - days{1});
    }
    if (*from > *to) {
        err << "meerkat: --from (" << day_text(*from) << ") is after --to (" << day_text(*to) << ")\n";
        return std::nullopt;
    }
    if (*to - *from >= kMaxSpan) {
        err << "meerkat: " << day_text(*from) << " to " << day_text(*to) << " is longer than "
            << kMaxSpan.count() << " days; make one report per year or less\n";
        return std::nullopt;
    }
    return ReportRange{*from, *to};
}

BuiltReport build_report(const core::Config& config, const ReportRange& range, const ReportEnv& env,
                         bool advice) {
    const auto& zone = env.zone;
    report::ReportMeta meta;
    meta.period = {zone.midnight(range.from), zone.midnight(range.to + days{1})};
    meta.first_day = range.from;
    meta.last_day = range.to;
    meta.zone = zone.name;
    meta.generated = env.now();
    meta.version = env.version;
    meta.contract = config.contract;
    meta.speed_interval = config.speed_interval;

    // The tests around the period too: the outage replay needs to know
    // when they kept the line busy.
    const report::Period wide{meta.period.from - report::kReplayMargin, meta.period.to + report::kReplayMargin};
    const auto speed = report::load_speed(config.data_dir, wide);
    const auto round = worst_round_time(config);
    const auto context = report::replay_context(speed.rows, round, config.speed_interval,
                                                config.connectivity_interval, config.outage_after);
    const auto outages = report::load_outages(config.data_dir, meta.period, context);
    const auto analysis = report::analyse(speed, outages, config.contract, zone.local, meta.period);
    meta.manifests = manifests(config.data_dir, meta.period);
    meta.official = report::analyse_official(report::load_official(config.data_dir), speed, zone.local, meta.period);
    if (advice && (config.contract.download || config.contract.upload)) {
        // The same four weeks as `meerkat advise-bbm` (ending at --to).
        const auto first = report::advice_first_day(range.to, range.from);
        const report::Period recent{zone.midnight(first), meta.period.to};
        meta.advice = report::advise(speed, config.contract, zone.local, recent,
                                     zone.local(meta.generated).day + days{1});
    }

    BuiltReport r;
    r.html = report::render_html(meta, speed, outages, analysis, zone.local);
    r.summary = official_line(meta.official) + verdict_line("Download", analysis.download) + ".\n" +
                verdict_line("Upload", analysis.upload) + ".\n" +
                core::text::plural(analysis.outages.count, "outage") + ".\n";
    r.speed_file = speed.file_exists;
    return r;
}

ReportEnv fixed_clock(const ReportEnv& env) {
    ReportEnv fixed = env;
    fixed.now = [now = env.now()] { return now; };
    return fixed;
}

int report_command(const core::Config& config, const ReportOptions& options, const ReportEnv& clock_env,
                   std::ostream& out, std::ostream& err) {
    const auto env = fixed_clock(clock_env);  // one moment for the range and the page
    const auto range = report_range(config, options, env, err);
    if (!range) {
        return kUsageError;
    }
    const auto output =
        options.output.empty()
            ? std::filesystem::path{"meerkat-report-" + day_text(range->from) + "-to-" + day_text(range->to) + ".html"}
            : options.output;
    if (inside_data_dir(output, config.data_dir)) {
        err << "meerkat: won't write the report into the data directory "
            << core::text::visible(config.data_dir.string())
            << " (it holds the evidence; choose another place with -o)\n";
        return kUsageError;
    }
    try {
        const auto r = build_report(config, *range, env, options.advice);
        // 0600: the report has the contract figures and the line's data in it.
        sys::AtomicFile file{output};
        file.write(r.html);
        file.commit();
        out << "Wrote " << core::text::visible(output.string()) << " (" << day_text(range->from) << " to "
            << day_text(range->to) << ", " << env.zone.name << ").\n"
            << r.summary;
        if (!r.speed_file) {
            out << "No speed.csv in " << core::text::visible(config.data_dir.string()) << " (see --data-dir).\n";
        }
    } catch (const std::exception& e) {
        err << "meerkat: " << core::text::visible(e.what()) << '\n';
        return 1;
    }
    return 0;
}

}  // namespace meerkat::app
