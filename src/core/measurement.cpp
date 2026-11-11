/**
 * @file measurement.cpp
 * @brief Out-of-line members of the measurement records.
 */
#include "core/measurement.hpp"

namespace meerkat::core {

std::optional<double> PingResult::loss_percent() const {
    if (!sent || *sent <= 0 || !received) {
        return std::nullopt;
    }
    const int lost = *sent - *received;
    return 100.0 * static_cast<double>(lost < 0 ? 0 : lost) / static_cast<double>(*sent);
}

}  // namespace meerkat::core
