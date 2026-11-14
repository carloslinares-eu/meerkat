/**
 * @file csv.hpp
 * @brief Append-only CSV files with a versioned schema (RFC 4180).
 */
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "store/append_file.hpp"

/**
 * @namespace meerkat::store
 * @brief Persistent storage: the CSV data files, the raw speed-test log and
 *        the daily checksum manifest.
 */
namespace meerkat::store {

/**
 * @brief The layout of one CSV file: its name, version and columns.
 *
 * The first line of every file is `# meerkat <name> v<version>`, the second
 * the column names. A file is only ever appended to by a writer with exactly
 * the same schema (see CsvWriter). Changing the columns means a new version.
 */
struct Schema {
    std::string name;                  ///< Kind of file, e.g. `speed`; no spaces.
    int version = 1;                   ///< Bumped whenever the columns change.
    std::vector<std::string> columns;  ///< Column names, in order.
};

/**
 * @brief Thrown when an existing file has a different header than the writer's
 *        schema, so appending would mix two layouts in one file.
 */
class SchemaMismatch : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Line ending of every row and header line (RFC 4180 uses CRLF).
inline constexpr std::string_view kLineEnd = "\r\n";

/**
 * @brief Quotes a field if RFC 4180 requires it.
 *
 * Fields containing a comma, a double quote, CR or LF are enclosed in double
 * quotes, with every `"` doubled. Fields with leading or trailing spaces are
 * quoted too, so no reader trims them. Everything else is returned unchanged;
 * an empty field stays empty (that is how a missing value is stored).
 *
 * @param field The raw value.
 * @return The field as it goes into the file.
 */
std::string escape_field(std::string_view field);

/**
 * @brief Joins fields into one CSV record, terminated by kLineEnd.
 * @param fields The raw values; each is escaped with escape_field().
 * @return The record.
 */
std::string format_record(const std::vector<std::string>& fields);

/**
 * @brief The two header lines a file with @p schema starts with.
 * @param schema The schema.
 * @return `# meerkat <name> v<version>` and the column names, each ending in
 *         kLineEnd.
 */
std::string header_text(const Schema& schema);

/**
 * @brief Appends rows to one CSV file, durably.
 *
 * - A new or empty file gets the header first (header_text()).
 * - An existing file must start with exactly that header, or the constructor
 *   throws SchemaMismatch and the file is left untouched.
 * - If the file ends in a partial row (the previous run died mid-write), the
 *   row is ended first: an open quoted field is closed, and a lone CR gets
 *   its LF. The damage stays in that one row. A file holding only the start
 *   of the header (the creating run died while writing it) gets the header
 *   written again.
 * - If a write or sync fails, the writer closes itself: the file may end in
 *   part of a row, or Linux may have dropped earlier rows that weren't on disk
 *   yet. Open a new writer to go on; it repairs the torn row.
 * - Each row is written with one `write()` on an `O_APPEND` descriptor and
 *   then `fdatasync()`ed, so a row that write_row() returned for survives a
 *   power loss.
 * - The file is locked with `flock()`: a second writer on the same file
 *   (another meerkat instance) fails instead of interleaving rows.
 *
 * New files are created with mode 0640 (before the umask), and the directory
 * is synced so the new file itself survives a crash. The directory must
 * already exist.
 *
 * @warning Fields are stored as they are, so a text field starting with `=`,
 *          `+`, `-` or `@` (an error message, a server or network name) can
 *          run as a formula when a spreadsheet opens the file. The files are
 *          for meerkat's report and for scripts; open them in a spreadsheet
 *          as text (e.g. LibreOffice's import dialog), or use the report.
 */
class CsvWriter {
public:
    /**
     * @brief Opens (or creates) @p path for appending rows of @p schema.
     * @param path   The CSV file.
     * @param schema Its layout; must have at least one column.
     * @throws SchemaMismatch     if the file has a different header.
     * @throws std::invalid_argument if @p schema has no columns.
     * @throws std::system_error  if the file can't be opened, locked, read or
     *                            written, or another process holds the lock.
     */
    CsvWriter(std::filesystem::path path, Schema schema);

    /// Closes the file (which releases the lock).
    ~CsvWriter() = default;

    CsvWriter(const CsvWriter&) = delete;             ///< Owns a descriptor: not copyable.
    CsvWriter& operator=(const CsvWriter&) = delete;  ///< Owns a descriptor: not copyable.

    /**
     * @brief Takes over @p other's file; @p other is left closed.
     * @param other The writer to move from.
     */
    CsvWriter(CsvWriter&& other) noexcept = default;

    /**
     * @brief Closes this writer's file and takes over @p other's.
     * @param other The writer to move from.
     * @return This writer.
     */
    CsvWriter& operator=(CsvWriter&& other) noexcept = default;

    /**
     * @brief Appends one row and syncs it to disk.
     * @param fields One raw value per column; empty for "not measured".
     * @throws std::invalid_argument if the number of fields doesn't match the
     *                               schema, or the writer was moved from.
     * @throws std::system_error     if writing or syncing fails; the writer is
     *                               then closed, and every later call throws
     *                               std::invalid_argument.
     */
    void write_row(const std::vector<std::string>& fields);

    /// @return The file being written.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return file_.path(); }

    /// @return The schema of the file.
    [[nodiscard]] const Schema& schema() const noexcept { return schema_; }

private:
    /**
     * @brief Checks the header of a non-empty file and repairs a torn last row.
     * @param size The file's current size in bytes.
     */
    void adopt_existing(std::size_t size);

    Schema schema_;    ///< Its layout.
    AppendFile file_;  ///< The open, locked file (and its path); closed after a write error.
};

}  // namespace meerkat::store
