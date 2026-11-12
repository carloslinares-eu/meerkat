/**
 * @file csv.cpp
 * @brief Implementation of the CSV writer.
 */
#include "store/csv.hpp"

#include <algorithm>
#include <utility>

namespace meerkat::store {

std::string escape_field(std::string_view field) {
    const bool needs_quotes =
        field.find_first_of(",\"\r\n") != std::string_view::npos ||
        (!field.empty() && (field.front() == ' ' || field.back() == ' '));
    if (!needs_quotes) {
        return std::string{field};
    }
    std::string out;
    out.reserve(field.size() + 2);
    out += '"';
    for (const char c : field) {
        if (c == '"') {
            out += '"';
        }
        out += c;
    }
    out += '"';
    return out;
}

std::string format_record(const std::vector<std::string>& fields) {
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += escape_field(fields[i]);
    }
    out += kLineEnd;
    return out;
}

std::string header_text(const Schema& schema) {
    std::string out = "# meerkat " + schema.name + " v" + std::to_string(schema.version);
    out += kLineEnd;
    out += format_record(schema.columns);
    return out;
}

CsvWriter::CsvWriter(std::filesystem::path path, Schema schema) : schema_{std::move(schema)} {
    if (schema_.columns.empty()) {
        throw std::invalid_argument("CSV schema '" + schema_.name + "' has no columns");
    }
    file_ = AppendFile{std::move(path)};
    const std::size_t size = file_.size();
    if (size > 0) {
        adopt_existing(size);
    } else {
        file_.append(header_text(schema_));
    }
}

void CsvWriter::adopt_existing(std::size_t size) {
    const std::string expected = header_text(schema_);
    const std::string head = file_.read(0, expected.size());
    if (size < expected.size() && head == expected.substr(0, size)) {
        // The creating writer died while writing the header: nothing but a
        // piece of our own header is there, so write it again.
        file_.rewrite(expected);
        return;
    }
    if (head != expected) {
        // Show the file's first line, so the message says what is there.
        std::string first = head.substr(0, head.find_first_of("\r\n"));
        throw SchemaMismatch(file_.path().string() + " has a different schema: expected '# meerkat " +
                             schema_.name + " v" + std::to_string(schema_.version) +
                             "' with the same columns, found '" + first + "'");
    }

    // Find out how the rows end. Quoted fields may contain line breaks, so
    // the only way to know whether the file ends inside a quoted field is to
    // follow the quoting from the end of the header.
    enum class State { field, quoted, quote_in_quoted };
    State state = State::field;
    char last = '\n';
    constexpr std::size_t kChunk = 1u << 16;
    for (std::size_t offset = expected.size(); offset < size;) {
        const std::string chunk = file_.read(offset, std::min(kChunk, size - offset));
        if (chunk.empty()) {
            break;  // the file shrank under us; repair what we have seen
        }
        for (const char c : chunk) {
            switch (state) {
            case State::field:
                if (c == '"') {
                    state = State::quoted;
                }
                break;
            case State::quoted:
                if (c == '"') {
                    state = State::quote_in_quoted;
                }
                break;
            case State::quote_in_quoted:  // `""` is an escaped quote; anything else closed it
                state = c == '"' ? State::quoted : State::field;
                break;
            }
        }
        last = chunk.back();
        offset += chunk.size();
    }

    // The previous writer died mid-row: end that row so ours starts clean,
    // and the damage stays in that one row.
    if (state == State::quoted) {
        file_.append("\"" + std::string{kLineEnd});  // close the open field first
    } else if (last == '\r') {
        file_.append("\n");  // died between CR and LF
    } else if (last != '\n') {
        file_.append(kLineEnd);
    }
}

void CsvWriter::write_row(const std::vector<std::string>& fields) {
    if (!file_.is_open()) {
        throw std::invalid_argument("CsvWriter: write_row() on a closed writer (moved from, "
                                    "or closed by an earlier write error)");
    }
    if (fields.size() != schema_.columns.size()) {
        throw std::invalid_argument("CsvWriter: " + std::to_string(fields.size()) +
                                    " fields for " + std::to_string(schema_.columns.size()) +
                                    " columns of '" + schema_.name + "'");
    }
    file_.append(format_record(fields));
}

}  // namespace meerkat::store
