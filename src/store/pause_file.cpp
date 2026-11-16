/**
 * @file pause_file.cpp
 * @brief Implementation of the pause file.
 */
#include "store/pause_file.hpp"

#include <stdexcept>
#include <string>

#include "store/state_file.hpp"
#include "sys/atomic_file.hpp"

namespace meerkat::store {

std::optional<core::Timestamp> read_pause(const std::filesystem::path& data_dir) {
    const auto path = data_dir / kPauseFile;
    const auto text = read_state_file(path);
    if (!text) {
        return std::nullopt;
    }
    const auto until = core::parse_iso8601(*text);
    if (!until) {
        throw std::runtime_error(path.string() + " doesn't hold a time; `meerkat pause` or "
                                 "`meerkat resume` replaces it");
    }
    return until;
}

void write_pause(const std::filesystem::path& data_dir, core::Timestamp until) {
    const auto target = data_dir / kPauseFile;
    sys::AtomicFile file{target, 0644};  // rename() replaces a symlink itself, never its target
    file.write(core::to_iso8601(until) + "\n");
    file.commit(true);  // a failed directory sync is reported
}

bool remove_pause(const std::filesystem::path& data_dir) { return remove_state_file(data_dir / kPauseFile); }

}  // namespace meerkat::store
