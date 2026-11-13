/**
 * @file link_notices.hpp
 * @brief Routes link-state notices to their warning or notice output.
 */
#pragma once

#include <string>
#include <vector>

#include "core/link_guard.hpp"

/**
 * @namespace meerkat::app
 * @brief Application-level command helpers.
 */
namespace meerkat::app {

/**
 * @brief Sends every link notice to the matching output callback.
 * @tparam WarningHandler Callback that accepts a warning message.
 * @tparam NoticeHandler Callback that accepts a non-warning message.
 * @param notices The messages returned by the link guard.
 * @param warning_handler Receives each warning.
 * @param notice_handler Receives each notice that is not a warning.
 */
template <typename WarningHandler, typename NoticeHandler>
void log_link_notices(const std::vector<core::LinkNotice>& notices,
                      WarningHandler&& warning_handler, NoticeHandler&& notice_handler) {
    for (const auto& notice : notices) {
        if (notice.warning) {
            warning_handler(notice.text);
        } else {
            notice_handler(notice.text);
        }
    }
}

}  // namespace meerkat::app
