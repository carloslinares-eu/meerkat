/**
 * @file svg.cpp
 * @brief The inline-SVG charts.
 */
#include "report/svg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

#include "core/text.hpp"
#include "report/format.hpp"

namespace meerkat::report {

namespace {

using std::chrono::seconds;

// Margins shared by the time chart and the outage chart, so their x axes
// line up when the report puts them under each other.
constexpr double kLeft = 44;    ///< Room for the value ticks.
constexpr double kRight = 112;  ///< Room for the reference labels.
constexpr double kBottom = 24;  ///< Room for the x ticks.
constexpr double kTop = 24;     ///< Room for the unit and the legend.
constexpr double kLabelGap = 12;   ///< Least vertical distance of two reference labels.
constexpr double kDayLabelWidth = 58;  ///< Room one day label needs.
constexpr std::size_t kMaxSeries = 3;  ///< Colour slots that stay apart for everyone.

constexpr std::array<std::string_view, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/// @return `mk-s1` … `mk-s3` for the slot @p i (0-based).
std::string slot_class(std::size_t i) { return "mk-s" + std::to_string(i + 1); }

/// @return E.g. `Tue 3 Nov`.
std::string day_label(std::chrono::sys_days day) {
    const std::chrono::year_month_day ymd{day};
    const std::chrono::weekday wd{day};
    return weekday_name(wd.c_encoding()) + " " +
           std::to_string(static_cast<unsigned>(ymd.day())) + " " +
           std::string{kMonths[static_cast<unsigned>(ymd.month()) - 1]};
}

/// @return The local wall clock of @p t, e.g. `14:05`.
std::string clock_label(core::Timestamp t, const LocalTime& lt) {
    const auto wall = t + lt.offset;
    const auto since = wall - std::chrono::floor<std::chrono::days>(wall);
    const auto minutes = std::chrono::floor<std::chrono::minutes>(since).count();
    return two(minutes / 60) + ":" + two(minutes % 60);
}

/// @return E.g. `Tue 3 Nov 14:05`.
std::string moment_label(core::Timestamp t, const LocalOf& local) {
    const auto lt = local(t);
    return day_label(lt.day) + " " + clock_label(t, lt);
}

/// Value axis from 0 to `top`, with a tick every `step`.
struct Scale {
    double top = 1;   ///< Highest tick.
    double step = 1;  ///< Between ticks.
};

/// @return A scale with about four steps of 1, 2, 2.5 or 5 × 10ⁿ that
///         reaches @p max; every tick has at most one decimal.
Scale nice_scale(double max) {
    if (!(max > 0) || !std::isfinite(max)) {
        max = 1;
    }
    const double raw = max / 4;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / mag;
    double s = f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10;
    if (s == 2.5 && mag < 1) {
        s = 5;  // 0.25 would need two decimals; tick labels have one
    }
    Scale out;
    out.step = std::max(0.1, s * mag);
    out.top = std::ceil(max / out.step - 1e-9) * out.step;
    return out;
}

/// The plot area inside the margins.
struct Plot {
    double left = 0;    ///< x of the left edge.
    double top = 0;     ///< y of the top edge.
    double width = 0;   ///< Width.
    double height = 0;  ///< Height.

    /// @return y of the bottom edge (the baseline).
    [[nodiscard]] double bottom() const { return top + height; }
    /// @return x of the right edge.
    [[nodiscard]] double right() const { return left + width; }
};

/// Maps a period onto the x range of a plot.
struct TimeAxis {
    Period period;  ///< The period.
    Plot plot;      ///< The plot.

    /// @return x of @p t, clamped to the plot.
    [[nodiscard]] double x(core::Timestamp t) const {
        const auto span = std::max<double>(1, static_cast<double>((period.to - period.from).count()));
        const double f = static_cast<double>((t - period.from).count()) / span;
        return plot.left + std::clamp(f, 0.0, 1.0) * plot.width;
    }
};

/// Maps values onto the y range of a plot.
struct ValueAxis {
    Scale scale;  ///< The scale.
    Plot plot;    ///< The plot.

    /// @return y of @p v, clamped to the plot.
    [[nodiscard]] double y(double v) const {
        return plot.bottom() - std::clamp(v / scale.top, 0.0, 1.0) * plot.height;
    }
};

/// Gridlines, value ticks and the unit.
void draw_value_axis(Svg& svg, const ValueAxis& axis, const std::string& unit) {
    const auto& p = axis.plot;
    const int steps = static_cast<int>(std::lround(axis.scale.top / axis.scale.step));
    for (int i = 0; i <= steps; ++i) {
        const double v = axis.scale.step * i;
        const double y = axis.y(v);
        svg.line(p.left, y, p.right(), y, i == 0 ? "mk-axis" : "mk-grid");
        svg.text(p.left - 6, y + 4, svg_number(v), "mk-tick", Svg::Anchor::end);
    }
    if (!unit.empty()) {
        svg.text(p.left - 6, p.top - 10, unit, "mk-unit", Svg::Anchor::end);
    }
}

/// Day starts: the first moment of each local day inside the period,
/// found once per day from the zone's offset (so half-hour zones work).
std::vector<std::pair<core::Timestamp, std::chrono::sys_days>> day_starts(const Period& period,
                                                                          const LocalOf& local) {
    std::vector<std::pair<core::Timestamp, std::chrono::sys_days>> out;
    if (period.to <= period.from) {
        return out;
    }
    const auto first = local(period.from).day;
    const auto last = local(period.to - seconds{1}).day;
    for (auto day = first; day <= last; day += std::chrono::days{1}) {
        const core::Timestamp wall{day};
        // Local midnight in UTC: try the offset just before it, and once
        // more with the offset there if it differs (a switch at midnight).
        auto t = wall - local(wall - std::chrono::hours{24}).offset;
        const auto again = wall - local(t).offset;
        if (local(again).day == day) {
            t = again;
        }
        if (local(t).day != day) {
            continue;  // the day starts later (gap at midnight): no tick
        }
        if (period.from <= t && t < period.to) {
            out.emplace_back(t, day);
        }
    }
    return out;
}

/// A hairline per day, labels on as many as fit.
void draw_day_axis(Svg& svg, const TimeAxis& axis, const LocalOf& local) {
    const auto& p = axis.plot;
    const auto days = day_starts(axis.period, local);
    const auto fit = std::max<std::size_t>(1, static_cast<std::size_t>(p.width / kDayLabelWidth));
    const std::size_t every = std::max<std::size_t>(1, (days.size() + fit - 1) / fit);
    if (days.empty() && axis.period.to > axis.period.from) {
        svg.text(p.left, p.bottom() + 16, day_label(local(axis.period.from).day), "mk-tick");
        return;
    }
    for (std::size_t i = 0; i < days.size(); ++i) {
        const double x = axis.x(days[i].first);
        svg.line(x, p.top, x, p.bottom(), "mk-grid");
        if (i % every == 0 && x + kDayLabelWidth - 6 <= p.right() + kRight) {
            svg.line(x, p.bottom(), x, p.bottom() + 4, "mk-axis");
            svg.text(x + 3, p.bottom() + 16, day_label(days[i].second), "mk-tick");
        }
    }
}

/// Horizontal lines with labels at the right; labels too close are pushed
/// apart downwards.
void draw_references(Svg& svg, const ValueAxis& axis, std::vector<ReferenceLine> refs) {
    std::stable_sort(refs.begin(), refs.end(),
                     [](const ReferenceLine& a, const ReferenceLine& b) { return a.value > b.value; });
    const auto& p = axis.plot;
    // Label baselines: pushed down where they'd touch, then up from the
    // baseline if that ran past it.
    std::vector<double> ly(refs.size());
    for (std::size_t i = 0; i < refs.size(); ++i) {
        ly[i] = axis.y(refs[i].value) + 4;
        if (i > 0) {
            ly[i] = std::max(ly[i], ly[i - 1] + kLabelGap);
        }
    }
    for (std::size_t i = refs.size(); i-- > 0;) {
        const double limit = i + 1 < refs.size() ? ly[i + 1] - kLabelGap : p.bottom() + 4;
        ly[i] = std::min(ly[i], limit);
    }
    for (std::size_t i = 0; i < refs.size(); ++i) {
        const double y = axis.y(refs[i].value);
        svg.open_group("mk-ref", refs[i].label);
        svg.line(p.left, y, p.right(), y, "mk-ref-line");
        svg.text(p.right() + 6, ly[i], refs[i].label, "mk-ref-label");
        svg.close_group();
    }
}

/// @return The number of UTF-8 code points in @p s (an estimate of its width).
std::size_t code_points(std::string_view s) {
    return static_cast<std::size_t>(
        std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

/// @return Path data of a diamond around (@p cx, @p cy), @p r from the centre to each corner.
std::string diamond(double cx, double cy, double r) {
    return "M" + svg_number(cx) + " " + svg_number(cy - r) + "L" + svg_number(cx + r) + " " + svg_number(cy) + "L" +
           svg_number(cx) + " " + svg_number(cy + r) + "L" + svg_number(cx - r) + " " + svg_number(cy) + "Z";
}

/// One legend entry.
struct LegendEntry {
    std::string name;     ///< Its label.
    std::size_t slot = 0; ///< Its colour slot.
    bool marker = false;  ///< A diamond instead of a square.
};

/// Legend items (swatch + name) in one row from @p x, at baseline @p y.
void draw_legend(Svg& svg, double x, double y, const std::vector<LegendEntry>& entries) {
    svg.open_group("mk-legend");
    for (const auto& e : entries) {
        if (e.marker) {
            svg.path(diamond(x + 5, y - 4, 6), "mk-swatch " + slot_class(e.slot));
        } else {
            svg.rect(x, y - 9, 10, 10, "mk-swatch " + slot_class(e.slot));
        }
        svg.text(x + 14, y, e.name, "mk-legend-label");
        x += 14 + 6.2 * static_cast<double>(code_points(e.name)) + 16;
    }
    svg.close_group();
}

/// A legend of squares, one per slot in order.
void draw_legend(Svg& svg, double x, double y, const std::vector<std::string>& names) {
    std::vector<LegendEntry> entries;
    for (std::size_t i = 0; i < names.size(); ++i) {
        entries.push_back({names[i], i, false});
    }
    draw_legend(svg, x, y, entries);
}

/// A note in the middle of the plot.
void draw_empty(Svg& svg, const Plot& p, std::string_view text) {
    svg.text(p.left + p.width / 2, p.top + p.height / 2 + 4, text, "mk-empty", Svg::Anchor::middle);
}

/// @return Path data of a column with 4 px rounded top corners and a
///         square foot on the baseline.
std::string column_path(double x, double base, double w, double h) {
    const double r = std::min({4.0, h, w / 2});
    const double top = base - h;
    return "M" + svg_number(x) + " " + svg_number(base) + "V" + svg_number(top + r) + "A" +
           svg_number(r) + " " + svg_number(r) + " 0 0 1 " + svg_number(x + r) + " " +
           svg_number(top) + "H" + svg_number(x + w - r) + "A" + svg_number(r) + " " +
           svg_number(r) + " 0 0 1 " + svg_number(x + w) + " " + svg_number(top + r) + "V" +
           svg_number(base) + "Z";
}

/// @return The largest value of the chart's points and reference lines.
double max_value(const std::vector<ReferenceLine>& refs, double data_max) {
    for (const auto& r : refs) {
        data_max = std::max(data_max, r.value);
    }
    return data_max;
}

/// @return The colour slot of a scope: fixed, whichever scopes appear.
std::size_t scope_slot(core::OutageScope s) {
    switch (s) {
    case core::OutageScope::internet: return 0;
    case core::OutageScope::local:    return 1;
    case core::OutageScope::unknown:  return 2;
    }
    return 2;
}

}  // namespace

std::string escape_xml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':  out += "&amp;"; break;
        case '<':  out += "&lt;"; break;
        case '>':  out += "&gt;"; break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        case '\t':
        case '\n': out += c; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) {
                out += c;  // other C0 controls and DEL: not allowed in XML 1.0, dropped
            }
        }
    }
    return out;
}

std::string svg_number(double v) {
    if (!std::isfinite(v)) {
        return "0";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    std::string s = buf;
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, ".0") == 0) {
        s.resize(s.size() - 2);
    }
    if (s == "-0") {
        s = "0";
    }
    return s;
}

Svg::Svg(double width, double height, std::string_view title, std::string_view description) {
    out_ = "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"mk-chart\" viewBox=\"0 0 " +
           svg_number(width) + " " + svg_number(height) + "\" role=\"img\" aria-label=\"" +
           escape_xml(title) + "\">\n<title>" + escape_xml(title) + "</title>\n";
    if (!description.empty()) {
        out_ += "<desc>" + escape_xml(description) + "</desc>\n";
    }
}

void Svg::close(std::string_view name, std::string_view tooltip) {
    if (tooltip.empty()) {
        out_ += "/>\n";
    } else {
        out_ += "><title>" + escape_xml(tooltip) + "</title></" + std::string{name} + ">\n";
    }
}

void Svg::rect(double x, double y, double w, double h, std::string_view cls,
               std::string_view tooltip) {
    out_ += "<rect class=\"" + escape_xml(cls) + "\" x=\"" + svg_number(x) + "\" y=\"" +
            svg_number(y) + "\" width=\"" + svg_number(std::max(0.0, w)) + "\" height=\"" +
            svg_number(std::max(0.0, h)) + "\"";
    close("rect", tooltip);
}

void Svg::line(double x1, double y1, double x2, double y2, std::string_view cls) {
    out_ += "<line class=\"" + escape_xml(cls) + "\" x1=\"" + svg_number(x1) + "\" y1=\"" +
            svg_number(y1) + "\" x2=\"" + svg_number(x2) + "\" y2=\"" + svg_number(y2) + "\"/>\n";
}

void Svg::path(std::string_view d, std::string_view cls, std::string_view tooltip) {
    out_ += "<path class=\"" + escape_xml(cls) + "\" d=\"" + escape_xml(d) + "\"";
    close("path", tooltip);
}

void Svg::circle(double cx, double cy, double r, std::string_view cls, std::string_view tooltip) {
    out_ += "<circle class=\"" + escape_xml(cls) + "\" cx=\"" + svg_number(cx) + "\" cy=\"" +
            svg_number(cy) + "\" r=\"" + svg_number(r) + "\"";
    close("circle", tooltip);
}

void Svg::text(double x, double y, std::string_view text, std::string_view cls, Anchor anchor) {
    out_ += "<text class=\"" + escape_xml(cls) + "\" x=\"" + svg_number(x) + "\" y=\"" +
            svg_number(y) + "\"";
    if (anchor == Anchor::middle) {
        out_ += " text-anchor=\"middle\"";
    } else if (anchor == Anchor::end) {
        out_ += " text-anchor=\"end\"";
    }
    out_ += ">" + escape_xml(text) + "</text>\n";
}

void Svg::open_group(std::string_view cls, std::string_view tooltip) {
    out_ += "<g class=\"" + escape_xml(cls) + "\">\n";
    if (!tooltip.empty()) {
        out_ += "<title>" + escape_xml(tooltip) + "</title>\n";
    }
    ++groups_;
}

void Svg::close_group() {
    if (groups_ > 0) {
        out_ += "</g>\n";
        --groups_;
    }
}

std::string Svg::finish() {
    while (groups_ > 0) {
        close_group();
    }
    out_ += "</svg>\n";
    return std::move(out_);
}

std::string chart_style() {
    // Colours: the dataviz reference palette (categorical slots 1–3, which
    // stay apart for every pair under colour-vision deficiency, light and
    // dark steps), its chart surface, inks and hairlines.
    static constexpr std::string_view kLight =
        "--mk-surface:#fcfcfb;--mk-ink:#0b0b0b;--mk-ink-2:#52514e;--mk-muted:#898781;"
        "--mk-grid:#e1e0d9;--mk-axis:#c3c2b7;--mk-track:#f0efec;"
        "--mk-s1:#2a78d6;--mk-s2:#eb6834;--mk-s3:#1baf7a;";
    static constexpr std::string_view kDark =
        "--mk-surface:#1a1a19;--mk-ink:#f0efec;--mk-ink-2:#c3c2b7;--mk-muted:#898781;"
        "--mk-grid:#2c2c2a;--mk-axis:#383835;--mk-track:#262624;"
        "--mk-s1:#3987e5;--mk-s2:#d95926;--mk-s3:#199e70;";
    std::string css;
    css += ".mk-chart{" + std::string{kLight} +
           "display:block;width:100%;height:auto;background:var(--mk-surface);"
           "font:11px system-ui,-apple-system,\"Segoe UI\",sans-serif;"
           "font-variant-numeric:tabular-nums}\n";
    css += "@media screen and (prefers-color-scheme:dark){:root:where(:not([data-theme=\"light\"])) "
           ".mk-chart{" + std::string{kDark} + "}}\n";
    css += "@media screen{:root[data-theme=\"dark\"] .mk-chart{" + std::string{kDark} + "}}\n";
    css += ".mk-chart text{fill:var(--mk-muted)}\n"
           ".mk-chart .mk-unit,.mk-chart .mk-legend-label,.mk-chart .mk-ref-label{fill:var(--mk-ink-2)}\n"
           ".mk-chart .mk-empty{fill:var(--mk-ink-2);font-size:12px}\n"
           ".mk-grid{stroke:var(--mk-grid);stroke-width:1}\n"
           ".mk-axis{stroke:var(--mk-axis);stroke-width:1}\n"
           ".mk-ref-line{stroke:var(--mk-ink-2);stroke-width:1.5}\n"
           ".mk-track{fill:var(--mk-track)}\n"
           ".mk-line{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}\n"
           ".mk-line.mk-s1{stroke:var(--mk-s1)}.mk-line.mk-s2{stroke:var(--mk-s2)}"
           ".mk-line.mk-s3{stroke:var(--mk-s3)}\n"
           ".mk-s1:not(.mk-line){fill:var(--mk-s1)}.mk-s2:not(.mk-line){fill:var(--mk-s2)}"
           ".mk-s3:not(.mk-line){fill:var(--mk-s3)}\n"
           ".mk-dot{stroke:var(--mk-surface);stroke-width:2}\n"
           ".mk-hit{fill:transparent}\n"
           ".mk-hit:hover{fill:var(--mk-ink);fill-opacity:.06}\n"
           "@media print{.mk-chart{break-inside:avoid}}\n";
    return css;
}

std::string time_chart(const TimeChart& chart, const LocalOf& local, ChartSize size) {
    Svg svg{size.width, size.height, chart.title, chart.description};
    const Plot plot{kLeft, kTop, size.width - kLeft - kRight, size.height - kTop - kBottom};
    const std::size_t n = std::min(chart.series.size(), kMaxSeries);

    double data_max = 0;
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto& pt : chart.series[i].points) {
            if (chart.period.contains(pt.time) && std::isfinite(pt.value)) {
                data_max = std::max(data_max, pt.value);
                any = true;
            }
        }
    }
    const ValueAxis values{nice_scale(max_value(chart.references, data_max)), plot};
    const TimeAxis times{chart.period, plot};

    draw_value_axis(svg, values, chart.unit);
    draw_day_axis(svg, times, local);
    const auto slot = [&](std::size_t i) { return chart.series[i].slot.value_or(i) % kMaxSeries; };
    if (n >= 2) {
        std::vector<LegendEntry> entries;
        for (std::size_t i = 0; i < n; ++i) {
            entries.push_back({chart.series[i].name, slot(i), chart.series[i].markers});
        }
        draw_legend(svg, plot.left + 8, plot.top - 10, entries);
    }

    const auto tip = [&](const TimeSeries& s, const TimePoint& pt) {
        return s.name + ", " + moment_label(pt.time, local) + ": " + svg_number(pt.value) +
               (chart.unit.empty() ? "" : " " + chart.unit);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto& s = chart.series[i];
        if (s.markers) {
            continue;  // drawn last, on top
        }
        std::string d;
        std::vector<TimePoint> alone;  // points without a neighbour within max_gap
        std::optional<TimePoint> prev;
        bool prev_joined = false;
        for (const auto& pt : s.points) {
            if (!chart.period.contains(pt.time) || !std::isfinite(pt.value)) {
                continue;
            }
            const std::string xy = svg_number(times.x(pt.time)) + " " + svg_number(values.y(pt.value));
            const bool join = prev && pt.time - prev->time <= chart.max_gap;
            if (join) {
                if (!prev_joined) {
                    d += "M" + svg_number(times.x(prev->time)) + " " +
                         svg_number(values.y(prev->value));
                }
                d += "L" + xy;
            } else if (prev && !prev_joined) {
                alone.push_back(*prev);
            }
            prev_joined = join;
            prev = pt;
        }
        if (prev && !prev_joined) {
            alone.push_back(*prev);
        }
        svg.open_group("mk-series", s.name);
        if (!d.empty()) {
            svg.path(d, "mk-line " + slot_class(slot(i)));
        }
        for (const auto& pt : alone) {
            svg.circle(times.x(pt.time), values.y(pt.value), 4, "mk-dot " + slot_class(slot(i)), tip(s, pt));
        }
        svg.close_group();
    }
    draw_references(svg, values, chart.references);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& s = chart.series[i];
        if (!s.markers) {
            continue;
        }
        svg.open_group("mk-series", s.name);
        for (const auto& pt : s.points) {
            if (chart.period.contains(pt.time) && std::isfinite(pt.value)) {
                svg.path(diamond(times.x(pt.time), values.y(pt.value), 6), "mk-dot " + slot_class(slot(i)),
                         tip(s, pt));
            }
        }
        svg.close_group();
    }
    if (!any) {
        draw_empty(svg, plot, "No data in this period");
    }
    return svg.finish();
}

std::string hour_chart(const HourChart& chart, ChartSize size) {
    Svg svg{size.width, size.height, chart.title, chart.description};
    const Plot plot{kLeft, kTop, size.width - kLeft - kRight, size.height - kTop - kBottom};
    double data_max = 0;
    bool any = false;
    for (const auto& b : chart.bars) {
        if (b.value && std::isfinite(*b.value)) {
            data_max = std::max(data_max, *b.value);
            any = true;
        }
    }
    const ValueAxis values{nice_scale(max_value(chart.references, data_max)), plot};
    draw_value_axis(svg, values, chart.unit);

    const double band = plot.width / 24;
    const double w = std::min(16.0, band * 0.6);
    for (int h = 0; h < 24; ++h) {
        const double x0 = plot.left + band * h;
        if (h % 3 == 0) {
            svg.text(x0 + band / 2, plot.bottom() + 16, two(h), "mk-tick", Svg::Anchor::middle);
        }
        const auto& b = chart.bars[static_cast<std::size_t>(h)];
        std::string tip = hour_slot(h) + ": ";
        if (b.value && std::isfinite(*b.value)) {
            tip += svg_number(*b.value) + (chart.unit.empty() ? "" : " " + chart.unit);
        } else {
            tip += "no data";
        }
        if (!b.note.empty()) {
            tip += ", " + b.note;
        }
        svg.open_group("mk-hour", tip);
        if (b.value && std::isfinite(*b.value)) {
            // At least a 2 px stub, so a 0 reads differently from no data.
            const double height = std::max(2.0, plot.bottom() - values.y(*b.value));
            svg.path(column_path(x0 + (band - w) / 2, plot.bottom(), w, height), "mk-bar mk-s1");
        }
        svg.rect(x0, plot.top, band, plot.height, "mk-hit");
        svg.close_group();
    }
    draw_references(svg, values, chart.references);
    if (!any) {
        draw_empty(svg, plot, "No data in this period");
    }
    return svg.finish();
}

std::string outage_chart(const OutageChart& chart, const LocalOf& local, ChartSize size) {
    Svg svg{size.width, size.height, chart.title, chart.description};
    const Plot plot{kLeft, kTop + 8, size.width - kLeft - kRight, size.height - kTop - 8 - kBottom};
    const TimeAxis times{chart.period, plot};

    std::vector<std::string> names;
    for (const auto s : {core::OutageScope::internet, core::OutageScope::local,
                         core::OutageScope::unknown}) {
        names.push_back(scope_text(s));
    }
    draw_legend(svg, plot.left, kTop - 6, names);
    svg.rect(plot.left, plot.top, plot.width, plot.height, "mk-track");
    draw_day_axis(svg, times, local);

    std::size_t shown = 0;
    for (const auto& o : chart.outages) {
        const auto end = o.end.value_or(chart.period.to);
        if (end <= chart.period.from || o.start >= chart.period.to) {
            continue;
        }
        const double x1 = times.x(o.start);
        const double x2 = times.x(end);
        const double w = std::max(2.0, x2 - x1);
        std::string tip = "Outage " + moment_label(o.start, local) + " – ";
        if (o.end) {
            tip += moment_label(*o.end, local) + " (" + duration_text(*o.end - o.start) + ")";
        } else {
            tip += "still going at the end of the period";
        }
        tip += ", " + scope_text(o.scope);
        if (o.end && (o.end_reason != core::OutageEnd::recovered || o.unconfirmed_end)) {
            tip += ", end: " + core::to_string(o.end_reason) + (o.unconfirmed_end ? " (unconfirmed)" : "");
        }
        if (!o.reason.empty()) {
            tip += ": " + core::text::visible(o.reason);  // tool output: no control characters
        }
        svg.rect(std::min(x1, plot.right() - w), plot.top, w, plot.height,
                 "mk-outage " + slot_class(scope_slot(o.scope)), tip);
        ++shown;
    }
    if (shown == 0) {
        draw_empty(svg, plot, "No outages in this period");
    }
    return svg.finish();
}

}  // namespace meerkat::report
