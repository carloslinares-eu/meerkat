/**
 * @file proc_list.hpp
 * @brief The running processes, read from procfs.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace meerkat::sys {

/// One running process.
struct ProcessEntry {
    int pid = 0;       ///< Process id.
    /// File name of the program (argv[0] without its directory, e.g.
    /// `breitbandmessung`); empty if /proc/PID/cmdline is empty (kernel
    /// threads, zombies) or can't be read.
    std::string name;
    /// The kernel's name for it (/proc/PID/comm): the program's file name,
    /// cut to 15 characters (`breitbandmessun`).
    std::string comm;
};

/// Longest /proc/PID/comm, in characters (TASK_COMM_LEN − 1).
inline constexpr std::size_t kCommLength = 15;

/**
 * @brief Lists the processes in @p proc.
 *
 * Reads `cmdline` and `comm` of every numeric directory. A process that ends
 * while it is read is left out. With `hidepid` mounted, other users'
 * processes don't show up; meerkat then can't see them (see
 * core::OfficialGuard for the manual fallback).
 * @param proc The procfs mount; tests pass a fake tree.
 * @return The processes, in no particular order; empty if @p proc can't be
 *         read.
 */
std::vector<ProcessEntry> list_processes(const std::filesystem::path& proc = "/proc");

/**
 * @brief Checks whether @p p runs the program @p name.
 *
 * True if its argv[0] file name is @p name, or starts with @p name and a
 * space (Chromium-based apps such as the Breitbandmessung app rewrite the
 * cmdline of their helper processes to one line). Only for a process
 * without a cmdline it is its comm, compared with @p name cut to
 * kCommLength; a longer program name with the same first 15 characters
 * then matches too.
 * @param p    The process.
 * @param name Program file name, e.g. `breitbandmessung`.
 * @return True if it matches.
 */
bool process_is(const ProcessEntry& p, std::string_view name);

/**
 * @brief The names in @p names that one of @p processes runs.
 * @param processes The running processes (list_processes()).
 * @param names     Program file names to look for.
 * @return The names found, each once, in the order of @p names.
 */
std::vector<std::string> running_programs(const std::vector<ProcessEntry>& processes,
                                          const std::vector<std::string>& names);

}  // namespace meerkat::sys
