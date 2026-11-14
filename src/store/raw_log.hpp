/**
 * @file raw_log.hpp
 * @brief The audit log: every speed-test backend's output, unchanged, one
 *        JSON line per test in one file per UTC day.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/measurement.hpp"
#include "store/append_file.hpp"

namespace meerkat::store {

/**
 * @brief The day (UTC) a timestamp falls on, as `YYYY-MM-DD`.
 * @param t The timestamp.
 * @return The date; it names the raw log and the manifest of that day.
 */
std::string utc_day(core::Timestamp t);

/**
 * @brief Encodes bytes as standard base64 (RFC 4648, with padding).
 * @param data Bytes.
 * @return The text.
 */
std::string base64(std::string_view data);

/**
 * @brief The raw-log line for one speed test, without the line break.
 *
 * A JSON object with, in this order: `time` (ISO 8601 UTC), `backend`,
 * `status`, `error`, and the backend's output. The output goes into `output`
 * as a JSON string when it is valid UTF-8 (the usual case), so reading it
 * back gives the exact bytes. Otherwise it goes into `output_base64`, so even
 * broken output is kept byte for byte. That depends on the output alone:
 * invalid bytes in the other fields (an error text in a non-UTF-8 locale)
 * are replaced with U+FFFD.
 *
 * @param m The measurement; `result.raw` is the output.
 * @return One line of JSON (no newline inside: JSON escapes them).
 */
std::string raw_record(const core::Measurement<core::SpeedResult>& m);

/**
 * @brief Appends raw_record()s to `<dir>/<YYYY-MM-DD>.jsonl`.
 *
 * Each test goes into the file of the UTC day it started on; the file
 * switches when the day changes. Lines are written and synced like CSV rows
 * (see AppendFile): durable when append() returns, the file locked against a
 * second writer, and closed after a write error. A line torn by a crash is
 * ended with a newline when the file is opened again, so later lines stay
 * readable; a reader skips the broken one.
 */
class RawLog {
public:
    /**
     * @brief Uses @p dir for the daily files, creating it if needed.
     * @param dir Usually `<data dir>/raw`.
     * @throws std::filesystem::filesystem_error if @p dir can't be created.
     */
    explicit RawLog(std::filesystem::path dir);

    /**
     * @brief Appends the record of one speed test and syncs it.
     * @param m The measurement.
     * @throws std::system_error if the day's file can't be opened, locked or
     *                           written. The next call tries to open it again.
     */
    void append(const core::Measurement<core::SpeedResult>& m);

    /**
     * @brief The file for one day.
     * @param day `YYYY-MM-DD`.
     * @return `<dir>/<day>.jsonl`.
     */
    [[nodiscard]] std::filesystem::path file_for(std::string_view day) const;

private:
    std::filesystem::path dir_;  ///< Where the daily files go.
    std::string day_;            ///< Day of the open file; empty if none.
    AppendFile file_;            ///< The open file of day_.
};

}  // namespace meerkat::store
