/**
 * @file csv_reader.cpp
 * @brief Implementation of read_csv().
 */
#include "store/csv_reader.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>

#include "sys/regular_file.hpp"

namespace meerkat::store {

namespace {

/// Splits the byte stream into records; knows nothing about schemas.
class RecordParser {
public:
    /// Called with each record and whether it ended with a line end.
    using Sink = std::function<void(std::vector<std::string>&, bool complete)>;

    /**
     * @brief Sets where records go.
     * @param sink The receiver.
     */
    explicit RecordParser(Sink sink) : sink_{std::move(sink)} {}

    /**
     * @brief Feeds the next bytes, until @p stop is set.
     * @param data The bytes.
     * @param stop Set by the sink to stop.
     */
    void feed(std::string_view data, const bool& stop) {
        for (const char c : data) {
            if (stop) {
                return;
            }
            step(c);
        }
    }

    /// The end of the file: a record without its line end is incomplete.
    void finish() {
        if (state_ == State::cr) {
            end_record();
            return;
        }
        if (started_) {
            fields_.push_back(std::move(field_));
            sink_(fields_, false);
        }
    }

private:
    /// Where in a record the parser is.
    enum class State { field, quoted, quote_in_quoted, cr };

    /**
     * @brief Handles one byte.
     * @param c The byte.
     */
    void step(char c) {
        switch (state_) {
        case State::quoted:
            if (c == '"') {
                state_ = State::quote_in_quoted;
            } else {
                field_ += c;
            }
            return;
        case State::quote_in_quoted:
            if (c == '"') {  // an escaped quote
                field_ += '"';
                state_ = State::quoted;
                return;
            }
            state_ = State::field;  // the quote closed the field
            break;
        case State::cr:
            state_ = State::field;
            if (c == '\n') {
                end_record();
                return;
            }
            field_ += '\r';  // a lone CR is data
            break;
        case State::field:
            break;
        }
        started_ = true;
        switch (c) {
        case '"':
            state_ = State::quoted;
            break;
        case ',':
            fields_.push_back(std::move(field_));
            field_.clear();
            break;
        case '\r':
            state_ = State::cr;
            break;
        case '\n':
            end_record();
            break;
        default:
            field_ += c;
        }
    }

    /// A line end outside quotes: hands on the record.
    void end_record() {
        fields_.push_back(std::move(field_));
        field_.clear();
        sink_(fields_, true);
        fields_.clear();
        started_ = false;
        state_ = State::field;
    }

    Sink sink_;                        ///< Gets the records.
    State state_ = State::field;       ///< Parser state.
    std::string field_;                ///< The field being read.
    std::vector<std::string> fields_;  ///< The record being read.
    bool started_ = false;             ///< The current record has a byte.
};

}  // namespace

CsvReadStats read_csv(const std::filesystem::path& path, const Schema& schema,
                      const CsvRowFn& row) {
    CsvReadStats stats;
    const std::string magic = "# meerkat " + schema.name + " v" + std::to_string(schema.version);
    std::size_t index = 0;
    bool stop = false;
    const auto mismatch = [&](const std::string& found) {
        throw SchemaMismatch(path.string() + " has a different schema: expected '" + magic +
                             "' with its columns, found '" + found + "'");
    };
    RecordParser parser{[&](std::vector<std::string>& fields, bool complete) {
        const std::size_t i = index++;
        if (i == 0) {
            if (fields.size() != 1 || fields[0] != magic) {
                mismatch(fields.empty() ? std::string{} : fields[0]);
            }
            return;
        }
        if (i == 1) {
            if (fields != schema.columns) {
                mismatch("columns " + (fields.empty() ? std::string{} : fields[0] + ",…"));
            }
            return;
        }
        if (!complete || fields.size() != schema.columns.size()) {
            ++stats.skipped;
            return;
        }
        ++stats.rows;
        if (!row(fields)) {
            stop = true;
        }
    }};

    const sys::Fd fd = sys::open_regular_file(path);
    std::string buf(1u << 16, '\0');
    while (!stop) {
        const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            const int err = errno;
            throw std::system_error(err, std::generic_category(), "can't read " + path.string());
        }
        if (n == 0) {
            break;
        }
        parser.feed({buf.data(), static_cast<std::size_t>(n)}, stop);
    }
    if (!stop) {
        parser.finish();
    }
    if (index == 1) {
        mismatch("only the first line");  // the columns line is missing
    }
    return stats;
}

std::optional<CsvReadStats> read_csv_if_there(const std::filesystem::path& path, const Schema& schema,
                                              const CsvRowFn& row) {
    try {
        return read_csv(path, schema, row);
    } catch (const std::system_error& e) {
        if (e.code() == std::errc::no_such_file_or_directory) {
            return std::nullopt;
        }
        throw;
    }
}

std::size_t Columns::operator[](std::string_view name) const {
    const auto it = std::find(names_.begin(), names_.end(), name);
    if (it == names_.end()) {
        throw std::logic_error("no column '" + std::string{name} + "'");
    }
    return static_cast<std::size_t>(it - names_.begin());
}

}  // namespace meerkat::store
