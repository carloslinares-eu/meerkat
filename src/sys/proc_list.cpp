/**
 * @file proc_list.cpp
 * @brief Implementation of the procfs process list.
 */
#include "sys/proc_list.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>

namespace meerkat::sys {

namespace {

/**
 * @brief The whole of a small procfs file.
 * @param path The file.
 * @return Its content; empty if it can't be read.
 */
std::string slurp(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief The pid a /proc entry stands for.
 * @param name The entry's file name.
 * @return The pid, or 0 if it isn't a process directory.
 */
int pid_of(const std::string& name) {
    int pid = 0;
    const auto* end = name.data() + name.size();
    const auto [ptr, ec] = std::from_chars(name.data(), end, pid);
    return ec == std::errc{} && ptr == end && pid > 0 ? pid : 0;
}

}  // namespace

std::vector<ProcessEntry> list_processes(const std::filesystem::path& proc) {
    std::vector<ProcessEntry> out;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator{proc, ec};
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        ProcessEntry p;
        p.pid = pid_of(it->path().filename().string());
        if (p.pid == 0) {
            continue;
        }
        p.comm = slurp(it->path() / "comm");
        if (p.comm.empty()) {
            continue;  // gone, or not a process
        }
        if (p.comm.back() == '\n') {
            p.comm.pop_back();
        }
        const auto cmdline = slurp(it->path() / "cmdline");
        const std::string argv0 = cmdline.substr(0, cmdline.find('\0'));
        p.name = argv0.substr(argv0.find_last_of('/') + 1);  // npos + 1 == 0
        out.push_back(std::move(p));
    }
    return out;
}

bool process_is(const ProcessEntry& p, std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (!p.name.empty()) {
        // Chromium rewrites its helpers' cmdline to one line, argv[0] first:
        // "breitbandmessung --type=renderer …".
        return p.name == name || (p.name.size() > name.size() && p.name.starts_with(name) &&
                                  p.name[name.size()] == ' ');
    }
    return p.comm == name.substr(0, kCommLength);
}

std::vector<std::string> running_programs(const std::vector<ProcessEntry>& processes,
                                          const std::vector<std::string>& names) {
    std::vector<std::string> found;
    for (const auto& n : names) {
        const bool running = std::any_of(processes.begin(), processes.end(),
                                         [&n](const ProcessEntry& p) { return process_is(p, n); });
        if (running && std::find(found.begin(), found.end(), n) == found.end()) {
            found.push_back(n);
        }
    }
    return found;
}

}  // namespace meerkat::sys
