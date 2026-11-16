/**
 * @file zip.cpp
 * @brief Implementation of the ZIP writer.
 */
#include "store/zip.hpp"

#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "sys/regular_file.hpp"

namespace meerkat::store {

namespace {

/// Highest value of a 32-bit field.
constexpr std::uint64_t kMax32 = std::numeric_limits<std::uint32_t>::max();
/// Chunk size for reading and deflating.
constexpr std::size_t kChunk = 1 << 16;
/// "Version needed to extract": 2.0 (deflate, data descriptor).
constexpr std::uint16_t kVersion = 20;
/// General purpose flags: bit 3 (sizes in a data descriptor), bit 11 (UTF-8 names).
constexpr std::uint16_t kFlags = (1u << 3) | (1u << 11);
/// Compression method 8: deflate.
constexpr std::uint16_t kDeflate = 8;

/// Appends a little-endian 16-bit value.
void put16(std::string& s, std::uint64_t v) {
    s += static_cast<char>(v & 0xff);
    s += static_cast<char>((v >> 8) & 0xff);
}

/// Appends a little-endian 32-bit value.
void put32(std::string& s, std::uint64_t v) {
    put16(s, v & 0xffff);
    put16(s, (v >> 16) & 0xffff);
}

/// @return @p v, or throws if it doesn't fit 32 bits.
std::uint64_t fit32(std::uint64_t v, const char* what) {
    if (v > kMax32) {
        throw std::length_error(std::string{what} + " is over 4 GiB, more than a ZIP without ZIP64 holds");
    }
    return v;
}

/**
 * @brief Whether @p name is a plain relative path for an archive entry.
 * @param name The name.
 * @return True if it is not empty, has no leading `/`, no `.`/`..`/empty
 *         parts, no backslash and no control characters.
 */
bool plain_name(std::string_view name) {
    if (name.empty() || name.size() > 0xffff || name.front() == '/' || name.back() == '/') {
        return false;
    }
    if (std::any_of(name.begin(), name.end(),
                    [](char c) { return c == '\\' || static_cast<unsigned char>(c) < 0x20 || c == 0x7f; })) {
        return false;
    }
    std::size_t start = 0;
    for (;;) {
        const auto end = name.find('/', start);
        const auto part = name.substr(start, end == std::string_view::npos ? end : end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
}

}  // namespace

ZipWriter::ZipWriter(Sink sink, core::Timestamp time) : sink_{std::move(sink)} {
    using namespace std::chrono;
    const auto day = floor<days>(time);
    const year_month_day ymd{day};
    const hh_mm_ss hms{floor<seconds>(time - day)};
    const auto y = static_cast<unsigned>(std::clamp(static_cast<int>(ymd.year()), 1980, 2107));  // the DOS range
    dos_date_ = static_cast<std::uint16_t>(((y - 1980u) << 9) | (static_cast<unsigned>(ymd.month()) << 5) |
                                           static_cast<unsigned>(ymd.day()));
    const auto hh = static_cast<unsigned>(hms.hours().count());
    const auto mm = static_cast<unsigned>(hms.minutes().count());
    const auto ss = static_cast<unsigned>(hms.seconds().count());
    dos_time_ = static_cast<std::uint16_t>((hh << 11) | (mm << 5) | (ss / 2));
}

void ZipWriter::emit(std::string_view bytes) {
    sink_(bytes);
    offset_ += bytes.size();
}

void ZipWriter::add(std::string_view name, std::string_view data) {
    std::size_t at = 0;
    add_stream(name, [&](std::vector<char>& buf) {
        const auto n = std::min(buf.size(), data.size() - at);
        std::copy_n(data.data() + at, n, buf.data());
        at += n;
        return n;
    });
}

void ZipWriter::add_file(std::string_view name, const std::filesystem::path& path, std::uint64_t size) {
    const auto fd = sys::open_regular_file(path);
    std::uint64_t left = size;
    add_stream(name, [&](std::vector<char>& buf) -> std::size_t {
        if (left == 0) {
            return 0;
        }
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
        for (;;) {
            const auto got = ::read(fd.get(), buf.data(), want);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got < 0) {
                throw std::system_error(errno, std::generic_category(), "can't read " + path.string());
            }
            if (got == 0) {
                throw std::runtime_error(path.string() + " got shorter while it was being exported");
            }
            left -= static_cast<std::uint64_t>(got);
            return static_cast<std::size_t>(got);
        }
    });
}

void ZipWriter::add_stream(std::string_view name, const std::function<std::size_t(std::vector<char>&)>& read) {
    if (finished_) {
        throw std::logic_error("ZipWriter: add after finish");
    }
    if (!plain_name(name)) {
        throw std::invalid_argument("not a plain relative name for a ZIP entry: " + std::string{name});
    }
    if (entries_.size() >= 0xffff) {
        throw std::length_error("more than 65535 files, more than a ZIP without ZIP64 holds");
    }
    Entry e{std::string{name}, 0, 0, 0, fit32(offset_, "the archive")};

    std::string head;
    put32(head, 0x04034b50);
    put16(head, kVersion);
    put16(head, kFlags);
    put16(head, kDeflate);
    put16(head, dos_time_);
    put16(head, dos_date_);
    put32(head, 0);  // CRC and sizes follow in the data descriptor
    put32(head, 0);
    put32(head, 0);
    put16(head, e.name.size());
    put16(head, 0);  // no extra field
    head += e.name;
    emit(head);

    z_stream z{};
    if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {  // raw deflate
        throw std::runtime_error("zlib: deflateInit2 failed");
    }
    struct End {
        z_stream* z;
        ~End() { deflateEnd(z); }
    } end{&z};
    std::vector<char> in(kChunk);
    std::vector<unsigned char> out(kChunk);
    uLong crc = crc32(0L, Z_NULL, 0);
    int flush = Z_NO_FLUSH;
    while (flush != Z_FINISH) {
        const auto n = read(in);
        flush = n == 0 ? Z_FINISH : Z_NO_FLUSH;
        crc = crc32(crc, reinterpret_cast<const Bytef*>(in.data()), static_cast<uInt>(n));
        e.size += n;
        z.next_in = reinterpret_cast<Bytef*>(in.data());
        z.avail_in = static_cast<uInt>(n);
        do {
            z.next_out = out.data();
            z.avail_out = static_cast<uInt>(out.size());
            if (deflate(&z, flush) == Z_STREAM_ERROR) {
                throw std::runtime_error("zlib: deflate failed");
            }
            const auto have = out.size() - z.avail_out;
            e.compressed += have;
            emit({reinterpret_cast<const char*>(out.data()), have});
        } while (z.avail_out == 0);
    }
    e.crc = static_cast<std::uint32_t>(crc);
    fit32(e.size, e.name.c_str());
    fit32(e.compressed, e.name.c_str());

    std::string desc;
    put32(desc, 0x08074b50);
    put32(desc, e.crc);
    put32(desc, e.compressed);
    put32(desc, e.size);
    emit(desc);
    entries_.push_back(std::move(e));
}

void ZipWriter::finish() {
    if (finished_) {
        return;
    }
    const auto start = fit32(offset_, "the archive");
    std::string dir;
    for (const auto& e : entries_) {
        put32(dir, 0x02014b50);
        put16(dir, (3u << 8) | kVersion);  // made by: Unix, 2.0
        put16(dir, kVersion);
        put16(dir, kFlags);
        put16(dir, kDeflate);
        put16(dir, dos_time_);
        put16(dir, dos_date_);
        put32(dir, e.crc);
        put32(dir, e.compressed);
        put32(dir, e.size);
        put16(dir, e.name.size());
        put16(dir, 0);  // extra
        put16(dir, 0);  // comment
        put16(dir, 0);  // disk
        put16(dir, 0);  // internal attributes
        put32(dir, 0100444u << 16);  // external: a regular file, read-only (r--r--r--)
        put32(dir, e.offset);
        dir += e.name;
    }
    emit(dir);
    std::string tail;
    put32(tail, 0x06054b50);
    put16(tail, 0);
    put16(tail, 0);
    put16(tail, entries_.size());
    put16(tail, entries_.size());
    put32(tail, fit32(dir.size(), "the archive"));
    put32(tail, start);
    put16(tail, 0);  // no comment
    fit32(offset_ + tail.size(), "the archive");
    emit(tail);
    finished_ = true;
}

}  // namespace meerkat::store
