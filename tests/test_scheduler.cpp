/**
 * @file test_scheduler.cpp
 * @brief Tests for core::Scheduler and the slot arithmetic, with a fake clock.
 */
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "core/clock.hpp"
#include "core/scheduler.hpp"

using namespace meerkat::core;
using namespace std::chrono_literals;
using std::chrono::seconds;

namespace {

/// A wall-clock time on 2026-11-12 (UTC).
WallTime at(int h, int m, int s = 0, std::chrono::milliseconds ms = 0ms) {
    using namespace std::chrono;
    return sys_days{year{2026} / 11 / 12} + hours{h} + minutes{m} + seconds{s} + ms;
}

/// The Timestamp of at().
Timestamp ts(int h, int m, int s = 0) { return std::chrono::floor<seconds>(at(h, m, s)); }

/**
 * @brief A clock the test controls.
 *
 * Auto mode: sleep_until() jumps to the deadline (plus a one-off jump the
 * test can plant), so a single job runs through its schedule at once.
 * Time that passes (set() forward, sleeping) advances the monotonic clock
 * too; jumps (jump(), jump_on_next_sleep()) move only the wall clock, like
 * a suspend or a clock change.
 * Manual mode: sleep_until() blocks until the test moves the time (set()),
 * and wait_idle() tells when every job thread is asleep again.
 */
class FakeClock final : public Clock {
public:
    FakeClock(WallTime start, bool auto_advance) : now_{start}, auto_{auto_advance} {}

    WallTime now() override {
        std::lock_guard l{m_};
        return now_;
    }

    std::chrono::steady_clock::time_point steady_now() override {
        std::lock_guard l{m_};
        return steady_;
    }

    void sleep_until(WallTime deadline, const std::atomic<bool>& stop) override {
        std::unique_lock l{m_};
        ++sleeps_;
        if (throw_after_ > 0 && sleeps_ >= throw_after_) {
            throw std::runtime_error("clock broke");
        }
        if (auto_) {
            if (!stop.load()) {
                if (deadline > now_) {
                    steady_ += deadline - now_;
                    now_ = deadline;
                }
                now_ += std::exchange(jump_, WallTime::duration::zero());
            }
            return;
        }
        const auto id = ++next_id_;
        sleepers_[id] = deadline;
        idle_cv_.notify_all();
        cv_.wait(l, [&] { return now_ >= deadline || stop.load(); });
        sleepers_.erase(id);
        idle_cv_.notify_all();
    }

    void wake_all() override {
        { std::lock_guard l{m_}; }
        cv_.notify_all();
    }

    /// Sets the time (also from inside a job, to simulate its duration).
    /// Moving forward counts as time passing; moving back as a clock change.
    void set(WallTime t) {
        {
            std::lock_guard l{m_};
            if (t > now_) {
                steady_ += t - now_;
            }
            now_ = t;
        }
        cv_.notify_all();
    }

    /// Moves only the wall clock (suspend or clock change), e.g. during a run.
    void jump(WallTime::duration d) {
        {
            std::lock_guard l{m_};
            now_ += d;
        }
        cv_.notify_all();
    }

    /// Makes the @p n-th call of sleep_until() (and later ones) throw.
    void throw_on_sleep(int n) {
        std::lock_guard l{m_};
        throw_after_ = n;
    }

    /// Auto mode: the next sleep overshoots its deadline by @p d (negative: the clock went back).
    void jump_on_next_sleep(WallTime::duration d) {
        std::lock_guard l{m_};
        jump_ = d;
    }

    /// Manual mode: waits until @p n threads sleep with deadlines in the future.
    bool wait_idle(std::size_t n) {
        std::unique_lock l{m_};
        return idle_cv_.wait_for(l, 5s, [&] {
            return sleepers_.size() == n &&
                   std::all_of(sleepers_.begin(), sleepers_.end(),
                               [&](const auto& s) { return s.second > now_; });
        });
    }

    /// @return Calls to sleep_until() so far.
    int sleeps() {
        std::lock_guard l{m_};
        return sleeps_;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    WallTime now_;
    bool auto_;
    WallTime::duration jump_{};
    std::chrono::steady_clock::time_point steady_{};
    int throw_after_ = 0;
    std::map<int, WallTime> sleepers_;
    int next_id_ = 0;
    int sleeps_ = 0;
};

/// What a single-job run recorded.
struct Log {
    std::vector<Timestamp> slots;          ///< Slot of each run.
    std::vector<WallTime> started;         ///< Clock time at each run.
    std::vector<SchedulerEvent> events;    ///< Events, in order.
};

/**
 * @brief Runs one job in auto mode until it has run @p runs times.
 * @param clock    The clock (auto mode).
 * @param interval Job interval.
 * @param runs     Stop after this many runs.
 * @param body     Called on each run with the run index (0-based) and the slot.
 * @param offset   Job offset.
 * @return What happened.
 */
Log run_single(FakeClock& clock, seconds interval, int runs,
               const std::function<void(int, Timestamp)>& body = {}, seconds offset = 0s) {
    Log log;
    Scheduler* self = nullptr;
    Job job{"job", interval, offset, [&](Timestamp slot) {
                const int i = static_cast<int>(log.slots.size());
                log.slots.push_back(slot);
                log.started.push_back(clock.now());
                if (i + 1 >= runs) {
                    self->stop();
                }
                if (body) {
                    body(i, slot);
                }
            }};
    Scheduler s{{job}, clock, [&](const SchedulerEvent& e) { log.events.push_back(e); }};
    self = &s;
    s.run();
    return log;
}

}  // namespace

TEST_CASE("slot arithmetic") {
    CHECK(slot_at_or_after(at(12, 1, 10), 5min, 0s) == ts(12, 5));
    CHECK(slot_at_or_before(at(12, 1, 10), 5min, 0s) == ts(12, 0));
    CHECK(slot_at_or_after(at(12, 5), 5min, 0s) == ts(12, 5));  // exactly on a slot
    CHECK(slot_at_or_before(at(12, 5), 5min, 0s) == ts(12, 5));
    CHECK(slot_at_or_after(at(12, 5, 0, 1ms), 5min, 0s) == ts(12, 10));
    CHECK(slot_at_or_before(at(12, 4, 59, 999ms), 5min, 0s) == ts(12, 0));
    CHECK(slot_at_or_after(at(12, 0, 1), 30s, 0s) == ts(12, 0, 30));
    CHECK(slot_at_or_after(at(12, 0, 1), 60s, 15s) == ts(12, 0, 15));
    CHECK(slot_at_or_after(at(12, 0, 16), 60s, 15s) == ts(12, 1, 15));
    // Hourly slots stay on the hour; a 7-minute grid counts from the epoch.
    CHECK(slot_at_or_after(at(12, 0, 1), 1h, 0s) == ts(13, 0));
    const auto seven = slot_at_or_after(at(12, 0), 7min, 0s);
    CHECK(seven.time_since_epoch().count() % 420 == 0);
    // Before the epoch, rounding still goes down.
    const WallTime before_epoch = WallTime{} - 90s;
    CHECK(slot_at_or_before(before_epoch, 60s, 0s) == Timestamp{-120s});
    CHECK(slot_at_or_after(before_epoch, 60s, 0s) == Timestamp{-60s});
}

TEST_CASE("jobs run at their slots, lined up with the wall clock") {
    FakeClock clock{at(12, 1, 10), true};
    const auto log = run_single(clock, 5min, 3);
    CHECK(log.slots == std::vector<Timestamp>{ts(12, 5), ts(12, 10), ts(12, 15)});
    CHECK(log.started == std::vector<WallTime>{at(12, 5), at(12, 10), at(12, 15)});
    CHECK(log.events.empty());

    SUBCASE("starting exactly on a slot runs it at once") {
        FakeClock c{at(12, 5), true};
        CHECK(run_single(c, 5min, 1).slots.front() == ts(12, 5));
    }
    SUBCASE("with an offset") {
        FakeClock c{at(12, 0, 30), true};
        CHECK(run_single(c, 60s, 2, {}, 15s).slots ==
              std::vector<Timestamp>{ts(12, 1, 15), ts(12, 2, 15)});
    }
}

TEST_CASE("the schedule doesn't drift with the run time") {
    FakeClock clock{at(12, 0), true};
    // Every run takes 40 s; the next one still starts on the 5-minute mark.
    const auto log = run_single(clock, 5min, 4, [&](int, Timestamp slot) { clock.set(slot + 40s); });
    CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 5), ts(12, 10), ts(12, 15)});
    CHECK(log.events.empty());
}

TEST_CASE("an overrun skips the slots it covered and reports them") {
    SUBCASE("one slot") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp slot) {
            if (i == 0) {
                clock.set(slot + 5min + 30s);  // the 12:00 run ends at 12:05:30
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 10)});
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].job == "job");
        CHECK(log.events[0].kind == SchedulerEvent::Kind::overrun);
        CHECK(log.events[0].slot == ts(12, 5));
        CHECK(log.events[0].count == 1);
    }
    SUBCASE("several slots") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 30s, 2, [&](int i, Timestamp slot) {
            if (i == 0) {
                clock.set(slot + 100s);  // covers :30, 1:00, 1:30
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 2)});
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].slot == ts(12, 0, 30));
        CHECK(log.events[0].count == 3);
    }
    SUBCASE("ending exactly on the next slot is not an overrun") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp slot) {
            if (i == 0) {
                clock.set(slot + 5min);
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 5)});
        CHECK(log.events.empty());
    }
}

TEST_CASE("waking up late (suspend, clock jump) skips the missed slots and runs the latest") {
    FakeClock clock{at(12, 0), true};
    const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
        if (i == 0) {
            clock.jump_on_next_sleep(17min);  // the 12:05 wake-up comes at 12:22
        }
    });
    CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 20)});
    CHECK(log.started.back() == at(12, 22));
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == SchedulerEvent::Kind::late);
    CHECK(log.events[0].slot == ts(12, 5));
    CHECK(log.events[0].count == 3);  // 12:05, 12:10, 12:15

    SUBCASE("a little late is still the same slot") {
        FakeClock c{at(12, 0), true};
        const auto l = run_single(c, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                c.jump_on_next_sleep(2s);
            }
        });
        CHECK(l.slots.back() == ts(12, 5));
        CHECK(l.events.empty());
    }
}

TEST_CASE("a step back of up to an hour is waited out: no slot runs twice") {
    SUBCASE("while sleeping") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.jump_on_next_sleep(-20min);  // waiting for 12:05, it becomes 11:45
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 5)});
        CHECK(log.started.back() == at(12, 5));
        REQUIRE(log.events.size() == 1);  // once, not on every wake-up
        CHECK(log.events[0].kind == SchedulerEvent::Kind::clock_back);
        CHECK(log.events[0].slot == ts(12, 5));
        CHECK(log.events[0].count == 4);  // 11:45, 11:50, 11:55, 12:00 ran already
    }
    SUBCASE("while running") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.set(at(11, 40));
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 5)});
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].kind == SchedulerEvent::Kind::clock_back);
        CHECK(log.events[0].count == 5);
    }
    SUBCASE("exactly an hour is still waited out") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.set(at(11, 0));
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 5)});
    }
    SUBCASE("a long interval: the step plus the rest of the interval") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 2h, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.set(at(11, 30));
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(14, 0)});
    }
    SUBCASE("less than two intervals back while sleeping is noticed too") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.jump_on_next_sleep(-8min);  // waiting for 12:05, it becomes 11:57
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 5)});
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].kind == SchedulerEvent::Kind::clock_back);
        CHECK(log.events[0].count == 1);  // 12:00
    }
    SUBCASE("a step back during a run that still ends after its slot") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.set(at(12, 4));    // the run takes 4 minutes …
                clock.jump(-3min);       // … and the clock goes back 3 of them
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 5)});
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].kind == SchedulerEvent::Kind::clock_back);
        CHECK(log.events[0].slot == ts(12, 5));
        CHECK(log.events[0].count == 0);
    }
    SUBCASE("a step back after an overrun") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.set(at(12, 17));               // overrun: next is 12:20
                clock.jump_on_next_sleep(-11min);    // at 12:20 it becomes 12:09
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 10)});
        REQUIRE(log.events.size() == 2);
        CHECK(log.events[0].kind == SchedulerEvent::Kind::overrun);
        CHECK(log.events[1].kind == SchedulerEvent::Kind::clock_back);
        CHECK(log.events[1].slot == ts(12, 10));
        CHECK(log.events[1].count == 0);  // 12:05 was skipped, never ran
    }
}

TEST_CASE("a step back of over an hour follows the new time and says so") {
    SUBCASE("while sleeping") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.jump_on_next_sleep(-2h);  // waiting for 12:05, it becomes 10:05
            }
        });
        // Waiting would stop the measurements for two hours.
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(10, 5)});
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].kind == SchedulerEvent::Kind::clock_back_repeat);
        CHECK(log.events[0].slot == ts(10, 5));
        CHECK(log.events[0].count == 24);  // 10:05 … 12:00 run again
    }
    SUBCASE("while running") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.set(at(10, 31));
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(10, 35)});
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].kind == SchedulerEvent::Kind::clock_back_repeat);
        CHECK(log.events[0].count == 18);  // 10:35 … 12:00
    }
    SUBCASE("a long interval: 1.5 h back is over the limit") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 2h, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.set(at(10, 30));
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 0)});
        REQUIRE(log.events.size() == 1);
        CHECK(log.events[0].count == 1);
    }
    SUBCASE("every step is reported, also the same one twice") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 5min, 3, [&](int i, Timestamp) {
            if (i < 2) {
                clock.set(at(10, 0) - 2h * i);  // 12:00 → 10:00, then 10:00 → 8:00
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(10, 0), ts(8, 0)});
        REQUIRE(log.events.size() == 2);
        CHECK(log.events[0].kind == SchedulerEvent::Kind::clock_back_repeat);
        CHECK(log.events[1].kind == SchedulerEvent::Kind::clock_back_repeat);
    }
}

TEST_CASE("a small step back never runs a slot twice") {
    SUBCASE("while sleeping") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 60s, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.jump_on_next_sleep(-61s);  // waiting for 12:01, it becomes 11:59:59
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 1)});
        REQUIRE(log.events.size() == 1);  // a whole minute: worth a line in the log
        CHECK(log.events[0].kind == SchedulerEvent::Kind::clock_back);
        CHECK(log.events[0].count == 1);
    }
    SUBCASE("while running") {
        FakeClock clock{at(12, 0), true};
        const auto log = run_single(clock, 60s, 2, [&](int i, Timestamp) {
            if (i == 0) {
                clock.set(at(11, 59, 59, 500ms));  // NTP steps back half a second
            }
        });
        CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 1)});
        CHECK(log.events.empty());
    }
}

TEST_CASE("a suspend during a run is reported as late, not as an overrun") {
    FakeClock clock{at(12, 0), true};
    const auto log = run_single(clock, 5min, 2, [&](int i, Timestamp slot) {
        if (i == 0) {
            clock.set(slot + 20s);  // the test runs 20 s …
            clock.jump(2h);         // … and the laptop sleeps for 2 h
        }
    });
    CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(14, 5)});
    REQUIRE(log.events.size() == 1);
    CHECK(log.events[0].kind == SchedulerEvent::Kind::late);
    CHECK(log.events[0].slot == ts(12, 5));
    CHECK(log.events[0].count == 24);  // 12:05 … 14:00
}

TEST_CASE("a failing clock stops the scheduler and run() reports it") {
    FakeClock clock{at(12, 0), true};
    clock.throw_on_sleep(3);
    int runs = 0;
    Scheduler s{{Job{"j", 30s, 0s, [&](Timestamp) { ++runs; }}}, clock};
    CHECK_THROWS_WITH_AS(s.run(), "clock broke", std::runtime_error);
    CHECK(s.stopping());
    CHECK(runs == 3);  // 12:00 needs no sleep; sleeps 1 and 2 lead to 12:00:30 and 12:01
}

TEST_CASE("run() can't be called twice") {
    FakeClock clock{at(12, 0), true};
    Scheduler s{{Job{"j", 30s, 0s, [](Timestamp) {}}}, clock};
    s.stop();
    s.run();
    CHECK_THROWS_AS(s.run(), std::logic_error);
}

TEST_CASE("a job that throws is reported and keeps its schedule") {
    FakeClock clock{at(12, 0), true};
    const auto log = run_single(clock, 30s, 3, [](int i, Timestamp) {
        if (i == 0) {
            throw std::runtime_error("speedtest exploded");
        }
        if (i == 1) {
            throw 42;  // not a std::exception
        }
    });
    CHECK(log.slots == std::vector<Timestamp>{ts(12, 0), ts(12, 0, 30), ts(12, 1)});
    REQUIRE(log.events.size() == 2);
    CHECK(log.events[0].kind == SchedulerEvent::Kind::failed);
    CHECK(log.events[0].slot == ts(12, 0));
    CHECK(log.events[0].detail == "speedtest exploded");
    CHECK(log.events[1].detail == "unknown exception");
}

TEST_CASE("a throwing event handler doesn't stop the job") {
    FakeClock clock{at(12, 0), true};
    int runs = 0;
    Scheduler* self = nullptr;
    Job job{"j", 30s, 0s, [&](Timestamp) {
                if (++runs == 3) {
                    self->stop();
                }
                throw std::runtime_error("fail");
            }};
    Scheduler s{{job}, clock, [](const SchedulerEvent&) { throw std::runtime_error("logger"); }};
    self = &s;
    s.run();
    CHECK(runs == 3);
}

TEST_CASE("stop() before run() means nothing runs") {
    FakeClock clock{at(12, 0), true};
    int runs = 0;
    Scheduler s{{Job{"j", 30s, 0s, [&](Timestamp) { ++runs; }}}, clock};
    CHECK_FALSE(s.stopping());
    s.stop();
    CHECK(s.stopping());
    s.run();
    CHECK(runs == 0);
}

TEST_CASE("invalid jobs are rejected") {
    FakeClock clock{at(12, 0), true};
    const auto fn = [](Timestamp) {};
    CHECK_THROWS_AS(Scheduler({}, clock), std::invalid_argument);
    CHECK_THROWS_AS(Scheduler({Job{"a", 30s, 0s, {}}}, clock), std::invalid_argument);
    CHECK_THROWS_AS(Scheduler({Job{"a", 0s, 0s, fn}}, clock), std::invalid_argument);
    CHECK_THROWS_AS(Scheduler({Job{"a", -5s, 0s, fn}}, clock), std::invalid_argument);
    CHECK_THROWS_AS(Scheduler({Job{"a", 30s, 30s, fn}}, clock), std::invalid_argument);
    CHECK_THROWS_AS(Scheduler({Job{"a", 30s, -1s, fn}}, clock), std::invalid_argument);
    CHECK_THROWS_AS(Scheduler({Job{"a", 30s, 0s, fn}, Job{"a", 60s, 0s, fn}}, clock),
                    std::invalid_argument);
}

TEST_CASE("two jobs run independently: a long speed test doesn't delay connectivity") {
    FakeClock clock{at(11, 59, 50), false};
    std::mutex m;
    std::vector<Timestamp> conn;
    std::vector<Timestamp> speed;
    std::vector<SchedulerEvent> events;
    std::promise<void> release_speed;
    auto released = release_speed.get_future().share();

    Job connectivity{"connectivity", 30s, 0s, [&](Timestamp slot) {
                         std::lock_guard l{m};
                         conn.push_back(slot);
                     }};
    Job speed_job{"speed", 5min, 0s, [&](Timestamp slot) {
                      {
                          std::lock_guard l{m};
                          speed.push_back(slot);
                      }
                      if (slot == ts(12, 5)) {
                          released.wait();  // this speed test "runs" until the test releases it
                      }
                  }};
    Scheduler s{{connectivity, speed_job}, clock, [&](const SchedulerEvent& e) {
                    std::lock_guard l{m};
                    events.push_back(e);
                }};
    std::thread runner{[&] { s.run(); }};

    REQUIRE(clock.wait_idle(2));
    for (WallTime t = at(12, 0); t <= at(12, 10); t += 30s) {
        clock.set(t);
        if (t >= at(12, 5) && t < at(12, 6, 30)) {
            REQUIRE(clock.wait_idle(1));  // speed is busy; connectivity sleeps
        } else {
            REQUIRE(clock.wait_idle(2));
        }
        if (t == at(12, 6)) {
            clock.set(at(12, 6, 10));
            release_speed.set_value();  // the 12:05 test ends at 12:06:10
            REQUIRE(clock.wait_idle(2));
        }
    }
    s.stop();
    runner.join();

    std::vector<Timestamp> expected_conn;
    for (WallTime t = at(12, 0); t <= at(12, 10); t += 30s) {
        expected_conn.push_back(std::chrono::floor<seconds>(t));
    }
    CHECK(conn == expected_conn);  // every 30 s, also during the speed test
    CHECK(speed == std::vector<Timestamp>{ts(12, 0), ts(12, 5), ts(12, 10)});
    CHECK(events.empty());
}

TEST_CASE("stop() wakes sleeping jobs at once (real clock)") {
    SystemClock clock;
    std::atomic<int> runs{0};
    // Slots an hour apart, offset so none is due now: both jobs sleep.
    const auto now_s = std::chrono::floor<seconds>(clock.now()).time_since_epoch().count();
    const seconds offset{(now_s + 1800) % 3600};
    Scheduler s{{Job{"a", 1h, offset, [&](Timestamp) { ++runs; }},
                 Job{"b", 1h, offset, [&](Timestamp) { ++runs; }}},
                clock};
    std::thread runner{[&] { s.run(); }};
    std::this_thread::sleep_for(50ms);
    const auto t0 = std::chrono::steady_clock::now();
    s.stop();
    runner.join();
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(took < 500ms);  // not the next 1-second slice, let alone the slot
    CHECK(runs == 0);
}

TEST_CASE("stop() lets a running job finish, then nothing more runs") {
    FakeClock clock{at(12, 0), false};
    std::promise<void> started;
    std::promise<void> finish;
    auto finish_f = finish.get_future();
    std::atomic<int> runs{0};
    std::atomic<bool> finished{false};
    Scheduler s{{Job{"j", 30s, 0s, [&](Timestamp) {
                     if (++runs == 1) {
                         started.set_value();
                         finish_f.wait();
                         finished = true;
                     }
                 }}},
                clock};
    std::thread runner{[&] { s.run(); }};
    started.get_future().wait();
    s.stop();
    clock.set(at(12, 1));  // a slot is due, but stop() came first
    finish.set_value();
    runner.join();
    CHECK(finished);
    CHECK(runs == 1);
}

TEST_CASE("SystemClock::sleep_until returns at the deadline or within a second") {
    SystemClock clock;
    const std::atomic<bool> stop{false};
    auto t0 = std::chrono::steady_clock::now();
    clock.sleep_until(clock.now() + 30ms, stop);
    auto took = std::chrono::steady_clock::now() - t0;
    CHECK(took >= 25ms);
    CHECK(took < 500ms);

    t0 = std::chrono::steady_clock::now();
    clock.sleep_until(clock.now() - 1s, stop);  // past deadline: no wait
    CHECK(std::chrono::steady_clock::now() - t0 < 50ms);
}
