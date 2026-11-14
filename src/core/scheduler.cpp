/**
 * @file scheduler.cpp
 * @brief Implementation of the fixed-rate scheduler.
 */
#include "core/scheduler.hpp"

#include <algorithm>
#include <exception>
#include <set>
#include <stdexcept>
#include <thread>

namespace meerkat::core {

namespace {

using std::chrono::seconds;

/**
 * @brief Floor division that rounds towards minus infinity (C++ `/` truncates).
 * @param a Dividend.
 * @param b Divisor; > 0.
 * @return ⌊a / b⌋.
 */
constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

}  // namespace

Timestamp slot_at_or_before(WallTime t, std::chrono::seconds interval, std::chrono::seconds offset) {
    // Whole seconds, rounded down: a slot at 12:00:00 is "at or before" 12:00:00.4.
    const auto s = std::chrono::floor<seconds>(t).time_since_epoch().count();
    const std::int64_t k = floor_div(s - offset.count(), interval.count());
    return Timestamp{seconds{k * interval.count() + offset.count()}};
}

Timestamp slot_at_or_after(WallTime t, std::chrono::seconds interval, std::chrono::seconds offset) {
    const Timestamp before = slot_at_or_before(t, interval, offset);
    return before >= t ? before : before + interval;
}

Scheduler::Scheduler(std::vector<Job> jobs, Clock& clock, SchedulerEventHandler on_event)
    : jobs_{std::move(jobs)}, clock_{clock}, on_event_{std::move(on_event)} {
    if (jobs_.empty()) {
        throw std::invalid_argument("Scheduler: no jobs");
    }
    std::set<std::string> names;
    for (const auto& j : jobs_) {
        if (!j.run) {
            throw std::invalid_argument("Scheduler: job '" + j.name + "' has nothing to run");
        }
        if (j.interval <= seconds::zero()) {
            throw std::invalid_argument("Scheduler: job '" + j.name + "' needs a positive interval");
        }
        if (j.offset < seconds::zero() || j.offset >= j.interval) {
            throw std::invalid_argument("Scheduler: job '" + j.name +
                                        "' needs an offset in [0, interval)");
        }
        if (!names.insert(j.name).second) {
            throw std::invalid_argument("Scheduler: two jobs are called '" + j.name + "'");
        }
    }
}

void Scheduler::run() {
    if (started_.exchange(true)) {
        throw std::logic_error("Scheduler::run() called twice");
    }
    std::vector<std::thread> threads;
    threads.reserve(jobs_.size());
    const auto guarded = [this](const Job& job) {
        try {
            run_job(job);
        } catch (...) {
            // The clock or the sleep failed, not the job: without a working
            // clock nothing can be scheduled, so stop everything and let
            // run() report it.
            {
                std::lock_guard lock{error_mutex_};
                if (!error_) {
                    error_ = std::current_exception();
                }
            }
            stop();
        }
    };
    try {
        for (const auto& job : jobs_) {
            threads.emplace_back(guarded, std::cref(job));
        }
    } catch (...) {
        // Couldn't start a thread: stop the ones that did start, then report.
        stop();
        for (auto& t : threads) {
            t.join();
        }
        throw;
    }
    for (auto& t : threads) {
        t.join();
    }
    if (error_) {
        std::rethrow_exception(error_);
    }
}

void Scheduler::stop() {
    stop_.store(true);
    clock_.wake_all();
}

void Scheduler::emit(const SchedulerEvent& e) {
    if (!on_event_) {
        return;
    }
    try {
        on_event_(e);
    } catch (...) {
        // A failing logger must not take a job thread down.
    }
}

Scheduler::Resync Scheduler::resync(const Job& job, WallTime now,
                                    const std::optional<Timestamp>& last) {
    const Timestamp candidate = slot_at_or_after(now, job.interval, job.offset);
    if (!last || candidate > *last) {
        return {.next = candidate};
    }
    const std::int64_t ran_already = (*last - candidate) / job.interval + 1;
    if (*last - now <= kMaxWaitBack) {
        // Never run the same slot twice: wait for the one after the last.
        return {.next = *last + job.interval, .slots = ran_already};
    }
    return {.next = candidate, .slots = ran_already, .repeats = true};
}

void Scheduler::run_job(const Job& job) {
    using namespace std::chrono_literals;
    const seconds interval = job.interval;
    const seconds offset = job.offset;
    std::optional<Timestamp> last;  // last slot that ran

    // Every reading of the clock goes through read_now(). A step back shows
    // as the wall clock moving at least a second less than the monotonic
    // one since the previous reading (a suspend or a jump forward moves it
    // more, which the late/overrun checks below handle).
    WallTime seen = clock_.now();
    auto seen_steady = clock_.steady_now();
    Timestamp next = slot_at_or_after(seen, interval, offset);
    const auto read_now = [&] {
        const WallTime now = clock_.now();
        const auto steady = clock_.steady_now();
        const bool stepped_back = (now - seen) - (steady - seen_steady) <= -1s;
        seen = now;
        seen_steady = steady;
        if (stepped_back) {
            const Resync r = resync(job, now, last);
            if (last) {  // before the first run there is nothing to protect
                emit({job.name,
                      r.repeats ? SchedulerEvent::Kind::clock_back_repeat
                                : SchedulerEvent::Kind::clock_back,
                      r.next, r.slots, {}});
            }
            next = r.next;
        }
        return std::pair{now, stepped_back};
    };

    while (!stop_.load()) {
        // Sleep until the slot. Re-read the clock after every wake-up: it
        // may have jumped (read_now() then moves `next`), and the sleep may
        // end early.
        WallTime now = read_now().first;
        while (now < next) {
            if (stop_.load()) {
                return;
            }
            clock_.sleep_until(next, stop_);
            now = read_now().first;
        }
        if (stop_.load()) {
            return;
        }

        // Normally `due == next`. After a suspend or a jump forward, whole
        // slots went by: skip them and run the latest one.
        const Timestamp due = slot_at_or_before(now, interval, offset);
        if (due > next) {
            emit({job.name, SchedulerEvent::Kind::late, next, (due - next) / interval, {}});
        }

        const auto steady_start = clock_.steady_now();
        try {
            job.run(due);
        } catch (const std::exception& e) {
            emit({job.name, SchedulerEvent::Kind::failed, due, 1, e.what()});
        } catch (...) {
            emit({job.name, SchedulerEvent::Kind::failed, due, 1, "unknown exception"});
        }
        last = due;

        const auto [end, stepped_back] = read_now();
        if (stepped_back) {
            continue;  // the clock went back during the run; read_now() set `next`
        }
        const Timestamp following = due + interval;
        next = std::max(following, slot_at_or_after(end, interval, offset));
        if (next > following) {
            // Did the run take that long, or did the wall clock move without
            // it (suspend, jump forward)? The monotonic clock tells.
            const auto ran = clock_.steady_now() - steady_start;
            const auto wall = end - now;
            const auto kind = wall - ran > 1s ? SchedulerEvent::Kind::late
                                              : SchedulerEvent::Kind::overrun;
            emit({job.name, kind, following, (next - following) / interval, {}});
        }
    }
}

}  // namespace meerkat::core
