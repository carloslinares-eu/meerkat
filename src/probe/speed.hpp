/**
 * @file speed.hpp
 * @brief Interface for speed-test backends.
 */
#pragma once

#include <string_view>

#include "core/measurement.hpp"
#include "sys/process.hpp"

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
     * Blocks until the test ends, times out or @p stop asks to end it. Every
     * failure is reported via core::SpeedResult::status and
     * core::SpeedResult::error.
     *
     * @param stop Asked while the test runs; true ends it early.
     * @return The result; `backend` is set to name().
     * @note Never throws.
     */
    [[nodiscard]] virtual core::SpeedResult measure(const sys::StopCheck& stop) = 0;

    /**
     * @brief Runs one speed test to its end.
     * @return The result, as measure(const sys::StopCheck&).
     */
    [[nodiscard]] core::SpeedResult measure() { return measure(sys::StopCheck{}); }
};

}  // namespace meerkat::probe
