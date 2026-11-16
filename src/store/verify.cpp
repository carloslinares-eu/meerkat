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

#include "core/text.hpp"
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
std::string day_text(sys_days d) { return core::to_iso_day(d); }

/// @return "D" for one day, "A to B" for more.
std::string range_text(sys_days a, sys_days b) {
    return a == b ? day_text(a) : day_text(a) + " to " + day_text(b);
}

/// @return "1 day" or "N days" for the days from @p a to @p b, both included.
std::string day_count(sys_days a, sys_days b) {
    return core::text::plural(static_cast<std::size_t>((b - a).count() + 1), "day");
}

/**
 * @brief Reads and parses one manifest file (its entries are checked later).
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
    auto file = read_manifest_file(data_dir, day);
    c.sha256 = std::move(file.sha256);
    c.manifest = std::move(file.manifest);
    if (!file.problem.empty()) {
        findings.push_back({VerifyFinding::Level::problem, day, name + " " + file.problem});
        return c;
    }
    if (c.manifest->day != day) {
        findings.push_back({VerifyFinding::Level::problem, day,
                            name + " says it is the manifest of " + c.manifest->day +
                                ": renamed or replaced"});
    }
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
                // Its entry is `missing` already, which counts as the problem.
                findings.push_back({VerifyFinding::Level::note, c.day,
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
 *        days after the last, and manifests dated today or later.
 * @param days_sealed The days with a manifest, sorted.
 * @param first       The first day with data, if any.
 * @param today       Today (UTC).
 * @param findings    Where they go.
 */
void check_days(const std::vector<sys_days>& days_sealed, std::optional<sys_days> first, sys_days today,
                std::vector<VerifyFinding>& findings) {
    const sys_days yesterday = today - days{1};
    // A day is sealed after it ends: a manifest of today or later was
    // written while the clock was wrong. `meerkat run` continues the chain
    // from the newest manifest, so it seals nothing until the clock gets there.
    std::vector<sys_days> past;
    for (const auto d : days_sealed) {
        if (d < today) {
            past.push_back(d);
        } else {
            findings.push_back({VerifyFinding::Level::problem, day_text(d),
                                manifest_path({}, day_text(d)).string() +
                                    " is dated today or later: it was written while the clock was "
                                    "wrong, and `meerkat run` seals no later day until the clock "
                                    "passes it"});
        }
    }
    if (past.empty() && !first) {
        if (days_sealed.empty()) {
            findings.push_back({VerifyFinding::Level::note, {}, "no data and no manifests yet"});
        }
        return;
    }
    if (!past.empty()) {
        if (first && *first < past.front()) {
            const auto b = past.front() - days{1};
            findings.push_back({VerifyFinding::Level::problem, day_text(*first),
                                range_text(*first, b) + " (" + day_count(*first, b) +
                                    ") have data but no manifest, before the first one: they can't "
                                    "be sealed any more"});
        }
        for (std::size_t i = 1; i < past.size(); ++i) {
            if (past[i] - past[i - 1] > days{1}) {
                const auto a = past[i - 1] + days{1};
                const auto b = past[i] - days{1};
                findings.push_back({VerifyFinding::Level::problem, day_text(a),
                                    "no manifest for " + range_text(a, b) + " (" + day_count(a, b) +
                                        "), between sealed days: they can't be sealed any more"});
            }
        }
    }
    // After the last manifest: `meerkat run` seals each day once it is over,
    // but only as far back as kSealBacklog.
    sys_days open = past.empty() ? *first : past.back() + days{1};
    const sys_days oldest_sealable = today - kSealBacklog;
    if (open < oldest_sealable && open < yesterday) {
        const auto b = std::min(oldest_sealable, yesterday) - days{1};
        findings.push_back({VerifyFinding::Level::problem, day_text(open),
                            range_text(open, b) + " (" + day_count(open, b) +
                                ") aren't sealed, and can't be any more: `meerkat run` only seals "
                                "the last " + std::to_string(kSealBacklog.count()) + " days"});
        open = b + days{1};
    }
    if (open < yesterday) {
        const auto b = yesterday - days{1};
        findings.push_back({VerifyFinding::Level::problem, day_text(open),
                            range_text(open, b) + " (" + day_count(open, b) +
                                ") aren't sealed: start `meerkat run`, it seals them"});
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
    std::vector<std::string> logs;
    for (const auto& e : raw_folder(data_dir)) {
        if (!e.day) {
            continue;
        }
        if (!e.regular) {
            findings.push_back({VerifyFinding::Level::problem, *e.day,
                                std::string{kRawDir} + "/" + e.name + " isn't a regular file"});
            continue;
        }
        logs.push_back(*e.day);
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
        const auto lists = [&path](const Manifest& m) {
            return std::any_of(m.entries.begin(), m.entries.end(),
                               [&path](const ManifestEntry& e) { return e.path == path; });
        };
        const auto next = by_day.find(day_text(d + days{1}));
        if (next != by_day.end()) {
            if (!lists(*next->second)) {
                findings.push_back({VerifyFinding::Level::problem, next->first,
                                    path + " isn't in the manifest of " + next->first +
                                        ": it was created after that day was sealed"});
            }
            continue;
        }
        // Not sealed for good yet (check_days() reports the day if it should
        // be). A test that started just before midnight may log after the
        // day's own manifest was written, so that alone is only a note.
        if (const auto own = by_day.find(day); own != by_day.end() && !lists(*own->second)) {
            findings.push_back({VerifyFinding::Level::note, day,
                                path + " was created after the manifest of " + day +
                                    " was written; the manifest of the next day seals it"});
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

std::map<std::string, std::vector<std::uint64_t>> VerifyResult::sealed_sizes() const {
    std::map<std::string, std::vector<std::uint64_t>> sizes;
    for (const auto& m : manifests) {
        if (m.manifest) {
            for (const auto& e : m.manifest->entries) {
                sizes[e.path].push_back(e.size);
            }
        }
    }
    return sizes;
}

ManifestFile read_manifest_file(const std::filesystem::path& data_dir, const std::string& day) {
    ManifestFile f;
    std::string text;
    try {
        text = sys::read_regular_file(manifest_path(data_dir, day), kMaxManifestBytes + 1);
    } catch (const std::system_error& e) {
        f.missing = e.code() == std::errc::no_such_file_or_directory;
        f.problem = std::string{"can't be read: "} + e.what();
        return f;
    } catch (const std::exception& e) {
        f.problem = std::string{"can't be read: "} + e.what();
        return f;
    }
    f.sha256 = sha256_hex(text);
    if (text.size() > kMaxManifestBytes) {
        f.problem = "is far too big to be a manifest";
        return f;
    }
    f.manifest = parse_manifest(text);
    if (!f.manifest) {
        f.problem = "isn't a valid meerkat manifest (edited, or not one)";
    }
    return f;
}

VerifyResult verify_data(const std::filesystem::path& data_dir, std::chrono::sys_days today) {
    VerifyResult r;
    r.today = today;
    std::vector<std::string> names;
    for (const auto& e : manifest_folder(data_dir)) {
        if (!e.day) {
            if (!(e.name.starts_with('.') && e.name.ends_with(".tmp"))) {  // write_manifest() at work
                r.findings.push_back({VerifyFinding::Level::note, {},
                                      std::string{kManifestDir} + "/" + e.name + " isn't a manifest; ignored"});
            }
            continue;
        }
        if (!e.regular) {
            r.findings.push_back({VerifyFinding::Level::problem, *e.day,
                                  std::string{kManifestDir} + "/" + e.name +
                                      " isn't a regular file, so it isn't a manifest"});
            continue;
        }
        names.push_back(*e.day);
    }
    std::sort(names.begin(), names.end());  // ISO dates sort as text

    std::vector<sys_days> sealed;
    std::map<std::string, const Manifest*> by_day;
    r.manifests.reserve(names.size());
    for (const auto& day : names) {
        r.manifests.push_back(check_one(data_dir, day, r.findings));
        sealed.push_back(parse_day(day).value());
    }
    // The CSVs are sealed at a new size every day: hash each once, taking
    // the digest at every sealed size, instead of once per manifest (#87).
    PrefixHashes known;
    for (auto& [path, list] : r.sealed_sizes()) {
        if (list.size() > 1) {
            known.emplace(path, sha256_prefixes(data_dir / path, std::move(list)));
        }
    }
    for (auto& m : r.manifests) {
        if (m.manifest) {
            m.entries = verify_manifest(data_dir, *m.manifest, &known);
        }
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
