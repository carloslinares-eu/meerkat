/**
 * @file svg.hpp
 * @brief Inline-SVG charts for the report: speed over time with the
 *        contract lines, the hour-of-day bars and the outage timeline.
 *
 * The charts carry no colours of their own, only CSS classes (`mk-…`); the
 * page that embeds them includes chart_style() once, which defines the
 * colours for light and dark mode and for print. Tooltips are SVG `<title>`
 * elements, so they work without JavaScript. Output is deterministic (one
 * decimal at most), which the golden-file tests rely on.
 */
#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/outage.hpp"
#include "core/time.hpp"
#include "report/data.hpp"
#include "report/stats.hpp"

namespace meerkat::report {

/**
 * @brief Escapes text for XML: `&`, `<`, `>`, `"` and `'`; drops the
 *        control characters XML 1.0 doesn't allow (all below 0x20 but tab
 *        and newline, and DEL).
 * @param text The text.
 * @return The escaped text.
 */
std::string escape_xml(std::string_view text);

/**
 * @brief A number for SVG coordinates and labels: at most one decimal,
 *        without a trailing `.0`, never `-0`.
 * @param v The number.
 * @return E.g. `12`, `12.5`, `-3.2`.
 */
std::string svg_number(double v);

/**
 * @brief Writes one `<svg>` element, element by element.
 *
 * Coordinates are in user units of the `viewBox`; the element itself scales
 * to the width of its container. Class names and text are escaped.
 */
class Svg {
public:
    /// Where a text sits relative to its x position.
    enum class Anchor {
        start,   ///< Text begins at x.
        middle,  ///< Text is centred on x.
        end,     ///< Text ends at x.
    };

    /**
     * @brief Starts the element.
     * @param width       Width of the viewBox.
     * @param height      Height of the viewBox.
     * @param title       Accessible name (`aria-label` and `<title>`).
     * @param description Longer description (`<desc>`); empty for none.
     */
    Svg(double width, double height, std::string_view title, std::string_view description);

    /**
     * @brief A rectangle.
     * @param x       Left edge.
     * @param y       Top edge.
     * @param w       Width.
     * @param h       Height.
     * @param cls     CSS class.
     * @param tooltip Tooltip; empty for none.
     */
    void rect(double x, double y, double w, double h, std::string_view cls,
              std::string_view tooltip = {});

    /**
     * @brief A straight line.
     * @param x1  Start x.
     * @param y1  Start y.
     * @param x2  End x.
     * @param y2  End y.
     * @param cls CSS class.
     */
    void line(double x1, double y1, double x2, double y2, std::string_view cls);

    /**
     * @brief A path.
     * @param d       Path data.
     * @param cls     CSS class.
     * @param tooltip Tooltip; empty for none.
     */
    void path(std::string_view d, std::string_view cls, std::string_view tooltip = {});

    /**
     * @brief A circle.
     * @param cx      Centre x.
     * @param cy      Centre y.
     * @param r       Radius.
     * @param cls     CSS class.
     * @param tooltip Tooltip; empty for none.
     */
    void circle(double cx, double cy, double r, std::string_view cls,
                std::string_view tooltip = {});

    /**
     * @brief A line of text; its baseline is at @p y.
     * @param x      Position, see @p anchor.
     * @param y      Baseline.
     * @param text   The text.
     * @param cls    CSS class.
     * @param anchor How it sits on @p x.
     */
    void text(double x, double y, std::string_view text, std::string_view cls,
              Anchor anchor = Anchor::start);

    /**
     * @brief Opens a `<g>`; close it with close_group().
     * @param cls     CSS class.
     * @param tooltip Tooltip for everything in it; empty for none.
     */
    void open_group(std::string_view cls, std::string_view tooltip = {});

    /// Closes the innermost open_group().
    void close_group();

    /**
     * @brief Closes the element (and any group left open).
     * @return The SVG markup, ending with a newline.
     */
    std::string finish();

private:
    /**
     * @brief Ends an element's start tag: `/>`, or `>` + its `<title>` +
     *        the end tag when it has a tooltip.
     * @param name    The element name.
     * @param tooltip The tooltip.
     */
    void close(std::string_view name, std::string_view tooltip);

    std::string out_;  ///< The markup so far.
    int groups_ = 0;   ///< Open groups.
};

/// A horizontal line for a contract figure, labelled at the right.
struct ReferenceLine {
    std::string label;  ///< E.g. `minimum 150`.
    double value = 0;   ///< On the value axis.
};

/// One value at one time.
struct TimePoint {
    core::Timestamp time{};  ///< When.
    double value = 0;        ///< What.
};

/// One series of a time chart.
struct TimeSeries {
    std::string name;              ///< For the legend and the tooltip.
    std::vector<TimePoint> points; ///< In time order.
};

/// Speed (or another value) over a period.
struct TimeChart {
    std::string title;               ///< Accessible name.
    std::string description;         ///< Longer description; may be empty.
    std::string unit;                ///< Of the values, e.g. `Mbit/s`.
    Period period;                   ///< The x axis.
    /// At most three; colours by position (more are left out). A legend is
    /// drawn for two or more.
    std::vector<TimeSeries> series;
    std::vector<ReferenceLine> references;  ///< Contract lines.
    /// Two points further apart than this aren't joined: the gap shows.
    std::chrono::seconds max_gap{std::chrono::minutes{15}};
};

/// One hour of the hour-of-day chart.
struct HourBar {
    std::optional<double> value;  ///< Bar height; empty: no data that hour.
    std::string note;             ///< Added to the tooltip, e.g. `12 tests`.
};

/// One value per local hour of the day.
struct HourChart {
    std::string title;                      ///< Accessible name.
    std::string description;                ///< Longer description; may be empty.
    std::string unit;                       ///< Of the values.
    std::array<HourBar, 24> bars{};         ///< Hour 0 to 23.
    std::vector<ReferenceLine> references;  ///< Contract lines.
};

/// The outages of a period on one line.
struct OutageChart {
    std::string title;                  ///< Accessible name.
    std::string description;            ///< Longer description; may be empty.
    Period period;                      ///< The x axis.
    /// The outages; open ones run to the end of the period, and the parts
    /// outside it are cut off.
    std::vector<core::Outage> outages;
};

/// Size of a chart's viewBox.
struct ChartSize {
    double width = 720;   ///< Width.
    double height = 260;  ///< Height.
};

/**
 * @brief The chart styles, for one `<style>` element on the page.
 *
 * Light colours by default, dark ones when the viewer prefers dark mode or
 * the page sets `data-theme="dark"` (but not `data-theme="light"`), and
 * always light in print.
 * @return CSS.
 */
std::string chart_style();

/**
 * @brief Values over time: one line per series (broken at gaps), the
 *        reference lines, local day ticks.
 * @param chart The data.
 * @param local Local day and UTC offset (it must set LocalTime::offset),
 *              for the ticks and the tooltips' wall clock.
 * @param size  The size.
 * @return An `<svg>` element.
 */
std::string time_chart(const TimeChart& chart, const LocalOf& local, ChartSize size = {});

/**
 * @brief One column per local hour, with the reference lines.
 * @param chart The data.
 * @param size  The size.
 * @return An `<svg>` element.
 */
std::string hour_chart(const HourChart& chart, ChartSize size = {720, 220});

/**
 * @brief The outages as blocks on a time line, coloured by scope, with a
 *        legend.
 * @param chart The data.
 * @param local Local day and UTC offset (it must set LocalTime::offset),
 *              for the ticks and the tooltips' wall clock.
 * @param size  The size.
 * @return An `<svg>` element.
 */
std::string outage_chart(const OutageChart& chart, const LocalOf& local,
                         ChartSize size = {720, 100});

}  // namespace meerkat::report
