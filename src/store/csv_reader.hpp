/**
 * @file csv_reader.hpp
 * @brief Reads meerkat's CSV files back, row by row (for the report).
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "store/csv.hpp"

namespace meerkat::store {

/// How a read went.
struct CsvReadStats {
    std::uint64_t rows = 0;     ///< Rows handed to the callback.
    /// Rows left out because they don't have one field per column: a row
    /// torn by a crash (CsvWriter ends it, but it stays short) or a last
    /// row without its line end (still being written).
    std::uint64_t skipped = 0;
};

/// Gets one row: one field per column of the schema, unescaped. Returns
/// false to stop reading (the rest of the file is left unread).
using CsvRowFn = std::function<bool(const std::vector<std::string>& fields)>;

/**
 * @brief Reads every row of a meerkat CSV, in file order.
 *
 * The file is read in chunks (it can be hundreds of MB), opened with
 * sys::open_regular_file(). The first two lines must be header_text() of
 * @p schema. Fields follow RFC 4180 as CsvWriter writes them: quoted when
 * they contain a comma, quote or line break, `""` for a quote; rows end
 * with CRLF (a bare LF is accepted too).
 *
 * @param path   The file.
 * @param schema Its expected layout.
 * @param row    Called for each complete row, until it returns false.
 * @return How many rows were read and skipped.
 * @throws SchemaMismatch        if the header isn't @p schema's (an empty
 *                               file counts as no rows).
 * @throws std::system_error     if the file can't be opened or read
 *                               (`ENOENT` if it doesn't exist).
 * @throws sys::NotRegularFile   if it isn't a regular file.
 */
CsvReadStats read_csv(const std::filesystem::path& path, const Schema& schema,
                      const CsvRowFn& row);

}  // namespace meerkat::store
