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
    std::vector<std::thread> threads;
    threads.reserve(jobs_.size());
    try {
        for (const auto& job : jobs_) {
            threads.emplace_back([this, &job] { run_job(job); });
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
}

void Scheduler::stop() {
    stop_.store(true);
    clock_.wake_all();
}

void Scheduler::emit(const SchedulerEvent& e) {
    if (!on_event_) {
        return;
    }
    std::lock_guard lock{event_mutex_};
    try {
        on_event_(e);
    } catch (...) {
        // A failing logger must not take a job thread down.
    }
}

void Scheduler::run_job(const Job& job) {
    const seconds interval = job.interval;
    const seconds offset = job.offset;
    Timestamp next = slot_at_or_after(clock_.now(), interval, offset);

    while (!stop_.load()) {
        // Sleep until the slot. Re-read the clock after every wake-up: it
        // may have jumped, and the sleep may end early.
        WallTime now = clock_.now();
        while (now < next) {
            if (stop_.load()) {
                return;
            }
            if (next - now > interval) {
                // The clock went back: the slot we waited for is now far in
                // the future. Wait for the next slot of the new time instead.
                next = slot_at_or_after(now, interval, offset);
            }
            clock_.sleep_until(next, stop_);
            now = clock_.now();
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

        try {
            job.run(due);
        } catch (const std::exception& e) {
            emit({job.name, SchedulerEvent::Kind::failed, due, 1, e.what()});
        } catch (...) {
            emit({job.name, SchedulerEvent::Kind::failed, due, 1, "unknown exception"});
        }

        const WallTime end = clock_.now();
        const Timestamp following = due + interval;
        if (end < due) {
            // The clock went back during the run.
            next = slot_at_or_after(end, interval, offset);
            continue;
        }
        next = std::max(following, slot_at_or_after(end, interval, offset));
        if (next > following) {
            emit({job.name, SchedulerEvent::Kind::overrun, following, (next - following) / interval,
                  {}});
        }
    }
}

}  // namespace meerkat::core
