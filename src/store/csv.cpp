/**
 * @file csv.cpp
 * @brief Implementation of the CSV writer.
 */
#include "store/csv.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <utility>

namespace meerkat::store {

namespace {

/**
 * @brief Builds a std::system_error from the current errno.
 * @param what Description of the failed step.
 * @param path The file involved.
 * @return The exception.
 */
std::system_error os_error(std::string_view what, const std::filesystem::path& path) {
    return {errno, std::generic_category(), std::string{what} + " " + path.string()};
}

/**
 * @brief Writes all of @p data to @p fd, retrying after EINTR and short writes.
 * @param fd   Descriptor.
 * @param data Bytes to write.
 * @return True on success; false with errno set.
 */
bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

/**
 * @brief Reads up to @p count bytes at @p offset.
 * @param fd     Descriptor.
 * @param offset Where to start.
 * @param count  Maximum number of bytes.
 * @param out    Receives the bytes (fewer at the end of the file).
 * @return True on success; false with errno set.
 */
bool read_at(int fd, off_t offset, std::size_t count, std::string& out) {
    out.assign(count, '\0');
    std::size_t got = 0;
    while (got < count) {
        const ssize_t n = ::pread(fd, out.data() + got, count - got,
                                  offset + static_cast<off_t>(got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            break;
        }
        got += static_cast<std::size_t>(n);
    }
    out.resize(got);
    return true;
}

/**
 * @brief fsync()s the directory containing @p path, so a new entry is durable.
 * @param path A file in the directory.
 * @return True on success; false with errno set.
 */
bool sync_parent(const std::filesystem::path& path) {
    auto dir = path.parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return false;
    }
    const bool ok = ::fsync(dfd) == 0;
    const int saved = errno;
    ::close(dfd);
    errno = saved;
    return ok;
}

}  // namespace

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

CsvWriter::CsvWriter(std::filesystem::path path, Schema schema)
    : path_{std::move(path)}, schema_{std::move(schema)} {
    if (schema_.columns.empty()) {
        throw std::invalid_argument("CSV schema '" + schema_.name + "' has no columns");
    }
    // O_EXCL first, so we know whether this call created the file. O_RDWR in
    // both cases: another process may have written to it before we got the
    // lock, and adopt_existing() reads it back.
    bool created = true;
    fd_ = ::open(path_.c_str(), O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
    if (fd_ < 0 && errno == EEXIST) {
        created = false;
        fd_ = ::open(path_.c_str(), O_RDWR | O_APPEND | O_CLOEXEC);
    }
    if (fd_ < 0) {
        throw os_error("can't open", path_);
    }
    try {
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            const int err = errno;  // before building the message, which may allocate
            if (err == EWOULDBLOCK) {
                throw std::system_error(err, std::generic_category(),
                                        "another process is writing " + path_.string());
            }
            throw std::system_error(err, std::generic_category(), "can't lock " + path_.string());
        }
        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            throw os_error("can't stat", path_);
        }
        if (!S_ISREG(st.st_mode)) {
            throw std::system_error(EINVAL, std::generic_category(),
                                    "not a regular file: " + path_.string());
        }
        if (st.st_size > 0) {
            adopt_existing(static_cast<std::size_t>(st.st_size));
        } else {
            if (!write_all(fd_, header_text(schema_)) || ::fsync(fd_) != 0) {
                throw os_error("can't write the header of", path_);
            }
        }
        if (created && !sync_parent(path_)) {
            throw os_error("can't sync the directory of", path_);
        }
    } catch (...) {
        close();
        throw;
    }
}

void CsvWriter::adopt_existing(std::size_t size) {
    const std::string expected = header_text(schema_);
    std::string head;
    if (!read_at(fd_, 0, expected.size(), head)) {
        throw os_error("can't read", path_);
    }
    if (size < expected.size() && head == expected.substr(0, size)) {
        // The creating writer died while writing the header: nothing but a
        // piece of our own header is there, so write it again.
        if (::ftruncate(fd_, 0) != 0 || !write_all(fd_, expected) || ::fsync(fd_) != 0) {
            throw os_error("can't rewrite the interrupted header of", path_);
        }
        return;
    }
    if (head != expected) {
        // Show the file's first line, so the message says what is there.
        std::string first = head.substr(0, head.find_first_of("\r\n"));
        throw SchemaMismatch(path_.string() + " has a different schema: expected '# meerkat " +
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
    std::string chunk;
    for (std::size_t offset = expected.size(); offset < size; offset += chunk.size()) {
        if (!read_at(fd_, static_cast<off_t>(offset), std::min(kChunk, size - offset), chunk)) {
            throw os_error("can't read", path_);
        }
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
    }

    // The previous writer died mid-row: end that row so ours starts clean,
    // and the damage stays in that one row.
    std::string repair;
    if (state == State::quoted) {
        repair = "\"" + std::string{kLineEnd};  // close the open field first
    } else if (last == '\r') {
        repair = "\n";  // died between CR and LF
    } else if (last != '\n') {
        repair = kLineEnd;
    }
    if (!repair.empty() && (!write_all(fd_, repair) || ::fsync(fd_) != 0)) {
        throw os_error("can't repair the last row of", path_);
    }
}

CsvWriter::~CsvWriter() { close(); }

CsvWriter::CsvWriter(CsvWriter&& other) noexcept
    : path_{std::move(other.path_)},
      schema_{std::move(other.schema_)},
      fd_{std::exchange(other.fd_, -1)} {}

CsvWriter& CsvWriter::operator=(CsvWriter&& other) noexcept {
    if (this != &other) {
        close();
        path_ = std::move(other.path_);
        schema_ = std::move(other.schema_);
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void CsvWriter::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void CsvWriter::write_row(const std::vector<std::string>& fields) {
    if (fd_ < 0) {
        throw std::invalid_argument("CsvWriter: write_row() on a closed writer (moved from, "
                                    "or closed by an earlier write error)");
    }
    if (fields.size() != schema_.columns.size()) {
        throw std::invalid_argument("CsvWriter: " + std::to_string(fields.size()) +
                                    " fields for " + std::to_string(schema_.columns.size()) +
                                    " columns of '" + schema_.name + "'");
    }
    // After a failed write the file may end in part of this row, and after a
    // failed sync Linux may have dropped earlier rows' pages and cleared the
    // error. Either way this descriptor can't be trusted any more: close it,
    // so the caller has to open a new writer, which repairs the torn row.
    if (!write_all(fd_, format_record(fields))) {
        const auto e = os_error("can't write to", path_);
        close();
        throw e;
    }
    if (::fdatasync(fd_) != 0) {
        const auto e = os_error("can't sync", path_);
        close();
        throw e;
    }
}

}  // namespace meerkat::store
