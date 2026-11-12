/**
 * @file speed.hpp
 * @brief Interface for speed-test backends.
 */
#pragma once

#include <string_view>

#include "core/measurement.hpp"

namespace meerkat::probe {

/**
 * @brief A tool that measures download and upload throughput.
 *
 * The default backend is OoklaBackend. Others (librespeed-cli, iperf3) can be
 * added later without touching the scheduler or the storage.
 */
class SpeedBackend {
public:
    /// Virtual destructor, so backends can be owned through this interface.
    virtual ~SpeedBackend() = default;

    /**
     * @brief Short, stable name of the backend, stored with every result.
     * @return For example `ookla`.
     */
    [[nodiscard]] virtual std::string_view name() const = 0;

    /**
     * @brief Runs one speed test.
     *
     * Blocks until the test ends or times out. Every failure is reported via
     * core::SpeedResult::status and core::SpeedResult::error.
     *
     * @return The result; `backend` is set to name().
     * @note Never throws.
     */
    [[nodiscard]] virtual core::SpeedResult measure() = 0;
};

}  // namespace meerkat::probe
