/**
 * @file verify.cpp
 * @brief Implementation of the evidence-trail check.
 */
#include "store/verify.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <string_view>
#include <system_error>

#include "store/raw_log.hpp"
#include "store/sha256.hpp"
#include "sys/regular_file.hpp"

namespace meerkat::store {

namespace fs = std::filesystem;

namespace {

using std::chrono::days;
using std::chrono::sys_days;

/// A manifest is a few hundred bytes; anything bigger than this isn't one.
constexpr std::size_t kMaxManifestBytes = 1 << 20;

/// @return `YYYY-MM-DD` of @p d.
std::string day_text(sys_days d) { return utc_day(core::Timestamp{d}); }

/// @return "D" for one day, "A to B" for more.
std::string range_text(sys_days a, sys_days b) {
    return a == b ? day_text(a) : day_text(a) + " to " + day_text(b);
}

/// @return "1 day" or "N days" for the days from @p a to @p b, both included.
std::string day_count(sys_days a, sys_days b) {
    const auto n = (b - a).count() + 1;
    return n == 1 ? "1 day" : std::to_string(n) + " days";
}

/**
 * @brief Reads and checks one manifest file.
 * @param data_dir The data directory.
 * @param day      Its day, from the file name.
 * @param findings Where a problem with the file itself goes.
 * @return The check.
 */
ManifestCheck check_one(const fs::path& data_dir, const std::string& day,
                        std::vector<VerifyFinding>& findings) {
    ManifestCheck c;
    c.day = day;
    const auto name = manifest_path({}, day).string();
    std::string text;
    try {
        text = sys::read_regular_file(manifest_path(data_dir, day), kMaxManifestBytes + 1);
    } catch (const std::exception& e) {
        findings.push_back({VerifyFinding::Level::problem, day, name + " can't be read: " + e.what()});
        return c;
    }
    c.sha256 = sha256_hex(text);
    if (text.size() > kMaxManifestBytes) {
        findings.push_back({VerifyFinding::Level::problem, day, name + " is far too big to be a manifest"});
        return c;
    }
    c.manifest = parse_manifest(text);
    if (!c.manifest) {
        findings.push_back({VerifyFinding::Level::problem, day,
                            name + " isn't a valid meerkat manifest (edited, or not one)"});
        return c;
    }
    if (c.manifest->day != day) {
        findings.push_back({VerifyFinding::Level::problem, day,
                            name + " says it is the manifest of " + c.manifest->day +
                                ": renamed or replaced"});
    }
    c.entries = verify_manifest(data_dir, *c.manifest);
    return c;
}

/**
 * @brief Checks that each manifest lists the one before it.
 * @param checks   The manifests, in date order.
 * @param findings Where problems go.
 */
void check_chain(const std::vector<ManifestCheck>& checks, std::vector<VerifyFinding>& findings) {
    const std::string prefix = std::string{kManifestDir} + "/";
    for (std::size_t i = 0; i < checks.size(); ++i) {
        const auto& c = checks[i];
        if (!c.manifest) {
            continue;  // already a problem; the next one is still checked against it
        }
        std::vector<std::string> listed;
        for (const auto& e : c.manifest->entries) {
            if (e.path.starts_with(prefix)) {
                listed.push_back(e.path);
            }
        }
        const auto here = manifest_path({}, c.day).string();
        if (listed.size() > 1) {
            findings.push_back({VerifyFinding::Level::problem, c.day,
                                here + " lists more than one earlier manifest: not written by meerkat"});
            continue;
        }
        if (i == 0) {
            if (!listed.empty()) {
                findings.push_back({VerifyFinding::Level::problem, c.day,
                                    here + " lists " + listed[0] +
                                        ", but there is no earlier manifest: the start of the chain "
                                        "was removed"});
            }
            continue;
        }
        const auto expected = manifest_path({}, checks[i - 1].day).string();
        if (listed.empty()) {
            findings.push_back({VerifyFinding::Level::problem, c.day,
                                here + " doesn't list the manifest before it (" + expected +
                                    "): the chain is broken here"});
        } else if (listed[0] != expected) {
            findings.push_back({VerifyFinding::Level::problem, c.day,
                                here + " lists " + listed[0] + ", but the manifest before it is " +
                                    expected + ": a manifest was added, removed or replaced later"});
        }
    }
}

/**
 * @brief Finds the days without a manifest: gaps, days before the first and
 *        days after the last.
 * @param days_sealed The days with a manifest, sorted.
 * @param first       The first day with data, if any.
 * @param today       Today (UTC).
 * @param findings    Where they go.
 */
void check_days(const std::vector<sys_days>& days_sealed, std::optional<sys_days> first, sys_days today,
                std::vector<VerifyFinding>& findings) {
    const sys_days yesterday = today - days{1};
    if (days_sealed.empty()) {
        if (!first) {
            findings.push_back({VerifyFinding::Level::note, {}, "no data and no manifests yet"});
            return;
        }
    } else {
        if (first && *first < days_sealed.front()) {
            const auto b = days_sealed.front() - days{1};
            findings.push_back({VerifyFinding::Level::problem, day_text(*first),
                                range_text(*first, b) + " (" + day_count(*first, b) +
                                    ") have data but no manifest, before the first one: they can't "
                                    "be sealed any more"});
        }
        for (std::size_t i = 1; i < days_sealed.size(); ++i) {
            if (days_sealed[i] - days_sealed[i - 1] > days{1}) {
                const auto a = days_sealed[i - 1] + days{1};
                const auto b = days_sealed[i] - days{1};
                findings.push_back({VerifyFinding::Level::problem, day_text(a),
                                    "no manifest for " + range_text(a, b) + " (" + day_count(a, b) +
                                        "), between sealed days: they can't be sealed any more"});
            }
        }
    }
    // After the last manifest: meerkat run seals each day once it is over.
    sys_days open = days_sealed.empty() ? *first : std::max(days_sealed.back() + days{1},
                                                                 first.value_or(days_sealed.back() + days{1}));
    if (open < yesterday) {
        const auto b = yesterday - days{1};
        findings.push_back({VerifyFinding::Level::problem, day_text(open),
                            range_text(open, b) + " (" + day_count(open, b) +
                                ") aren't sealed: `meerkat run` seals past days when it runs (the "
                                "last 31 days)"});
        open = yesterday;
    }
    if (open == yesterday) {
        findings.push_back({VerifyFinding::Level::note, day_text(yesterday),
                            "yesterday (" + day_text(yesterday) +
                                ", UTC) isn't sealed yet; `meerkat run` seals it soon after midnight UTC"});
    }
}

/**
 * @brief Checks the raw logs against the manifests of the day after them.
 * @param data_dir The data directory.
 * @param by_day   The readable manifests, by day.
 * @param today    Today (UTC).
 * @param findings Where problems and notes go.
 */
void check_raw_logs(const fs::path& data_dir, const std::map<std::string, const Manifest*>& by_day,
                    sys_days today, std::vector<VerifyFinding>& findings) {
    const fs::path dir = data_dir / kRawDir;
    if (!fs::exists(dir)) {  // throws for errors other than "not there"
        return;
    }
    std::vector<std::string> logs;
    for (const auto& e : fs::directory_iterator{dir}) {
        const auto name = e.path().filename().string();
        const auto day = raw_log_day(name);
        if (!day) {
            continue;
        }
        if (e.symlink_status().type() != fs::file_type::regular) {
            findings.push_back({VerifyFinding::Level::problem, *day,
                                std::string{kRawDir} + "/" + name + " isn't a regular file"});
            continue;
        }
        logs.push_back(*day);
    }
    std::sort(logs.begin(), logs.end());
    for (const auto& day : logs) {
        const auto path = std::string{kRawDir} + "/" + day + ".jsonl";
        const auto d = parse_day(day).value();
        if (d > today) {
            findings.push_back({VerifyFinding::Level::note, day,
                                path + " is dated after today: was the clock wrong?"});
            continue;
        }
        const auto next = by_day.find(day_text(d + days{1}));
        if (next == by_day.end()) {
            continue;  // unsealed or a gap: check_days() reports the day
        }
        const auto& entries = next->second->entries;
        const bool listed = std::any_of(entries.begin(), entries.end(),
                                        [&path](const ManifestEntry& e) { return e.path == path; });
        if (!listed) {
            findings.push_back({VerifyFinding::Level::problem, day,
                                path + " isn't in the manifest of " + next->first +
                                    ": it was created after that day was sealed"});
        }
    }
}

/**
 * @brief Notes what the CSVs got after the last manifest.
 * @param data_dir The data directory.
 * @param last     The last readable manifest, if any.
 * @param findings Where the notes go.
 */
void check_tails(const fs::path& data_dir, const Manifest* last, std::vector<VerifyFinding>& findings) {
    for (const auto file : {kSpeedFile, kConnectivityFile}) {
        std::error_code ec;
        const auto size = fs::file_size(data_dir / file, ec);
        if (ec) {
            continue;
        }
        std::uint64_t sealed = 0;
        if (last != nullptr) {
            for (const auto& e : last->entries) {
                if (e.path == file) {
                    sealed = e.size;
                }
            }
        }
        if (size > sealed) {
            findings.push_back({VerifyFinding::Level::note, {},
                                std::string{file} + ": " + std::to_string(size - sealed) +
                                    " bytes added since " +
                                    (last != nullptr ? "the last manifest (" + last->day + ")"
                                                     : std::string{"it was started"}) +
                                    ", not sealed yet"});
        }
    }
}

}  // namespace

std::size_t VerifyResult::problems() const {
    std::size_t n = 0;
    for (const auto& f : findings) {
        n += f.level == VerifyFinding::Level::problem ? 1 : 0;
    }
    for (const auto& m : manifests) {
        for (const auto& e : m.entries) {
            n += e.outcome == EntryCheck::Outcome::ok ? 0 : 1;
        }
    }
    return n;
}

VerifyResult verify_data(const std::filesystem::path& data_dir, std::chrono::sys_days today) {
    VerifyResult r;
    std::vector<std::string> names;
    const fs::path dir = data_dir / kManifestDir;
    if (fs::exists(dir)) {  // throws for errors other than "not there"
        for (const auto& e : fs::directory_iterator{dir}) {
            const auto name = e.path().filename().string();
            const auto day = manifest_day(name);
            if (!day) {
                if (!(name.starts_with('.') && name.ends_with(".tmp"))) {  // write_manifest() at work
                    r.findings.push_back({VerifyFinding::Level::note, {},
                                          std::string{kManifestDir} + "/" + name + " isn't a manifest; ignored"});
                }
                continue;
            }
            if (e.symlink_status().type() != fs::file_type::regular) {
                r.findings.push_back({VerifyFinding::Level::problem, *day,
                                      std::string{kManifestDir} + "/" + name +
                                          " isn't a regular file, so it isn't a manifest"});
                continue;
            }
            names.push_back(*day);
        }
    }
    std::sort(names.begin(), names.end());  // ISO dates sort as text

    std::vector<sys_days> sealed;
    std::map<std::string, const Manifest*> by_day;
    r.manifests.reserve(names.size());
    for (const auto& day : names) {
        r.manifests.push_back(check_one(data_dir, day, r.findings));
        sealed.push_back(parse_day(day).value());
    }
    const Manifest* last = nullptr;
    for (const auto& m : r.manifests) {
        if (m.manifest) {
            by_day.emplace(m.day, &*m.manifest);
            last = &*m.manifest;
        }
    }

    check_chain(r.manifests, r.findings);
    check_days(sealed, first_data_day(data_dir), today, r.findings);
    check_raw_logs(data_dir, by_day, today, r.findings);
    check_tails(data_dir, last, r.findings);
    std::stable_sort(r.findings.begin(), r.findings.end(), [](const VerifyFinding& a, const VerifyFinding& b) {
        return a.day < b.day;  // the ones without a day first
    });
    return r;
}

std::string outcome_name(EntryCheck::Outcome o) {
    switch (o) {
    case EntryCheck::Outcome::ok: return "ok";
    case EntryCheck::Outcome::changed: return "changed";
    case EntryCheck::Outcome::grew: return "grew";
    case EntryCheck::Outcome::missing: return "missing";
    case EntryCheck::Outcome::shorter: return "shorter";
    }
    return "?";
}

std::string outcome_meaning(EntryCheck::Outcome o) {
    switch (o) {
    case EntryCheck::Outcome::ok: return "unchanged since it was sealed";
    case EntryCheck::Outcome::changed: return "the sealed part was edited";
    case EntryCheck::Outcome::grew: return "lines were added after it was complete";
    case EntryCheck::Outcome::missing: return "gone, unreadable or not a regular file";
    case EntryCheck::Outcome::shorter: return "shorter than sealed: data was removed";
    }
    return "?";
}

}  // namespace meerkat::store
