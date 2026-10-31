#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

// Fixed-rate and fixed-delay periodic tasks.

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::waitFor;

namespace {
    /**
     * Counts the runs in progress and remembers the most at the same time.
     */
    struct Overlap
    {
        std::atomic<int> current{0};
        std::atomic<int> most{0};
        std::atomic<int> runs{0};

        void run(const std::chrono::milliseconds duration)
        {
            const int now = ++current;
            int previous = most.load();
            while (previous < now && !most.compare_exchange_weak(previous, now)) {}
            std::this_thread::sleep_for(duration);
            --current;
            ++runs;
        }
    };
} // namespace

TEST_CASE("Fixed-rate runs can overlap on a thread pool", "[periodic][fixed rate]")
{
    const auto pool = ThreadPoolExecutor::create(4);
    Overlap overlap;

    // Each run takes longer than the interval.
    auto handle = Task::run(pool.get(), 0ms, FixedRate{10ms}, [&overlap] { overlap.run(40ms); }).submit();

    REQUIRE(waitFor([&overlap] { return overlap.runs >= 6; }));
    handle.cancel();
    handle.wait();
    REQUIRE(overlap.most > 1);
}

TEST_CASE("Fixed-delay runs never overlap", "[periodic][fixed delay]")
{
    const auto pool = ThreadPoolExecutor::create(4);
    Overlap overlap;

    auto handle = Task::run(pool.get(), 0ms, FixedDelay{1ms}, [&overlap] { overlap.run(10ms); }).submit();

    REQUIRE(waitFor([&overlap] { return overlap.runs >= 5; }));
    handle.cancel();
    handle.wait();
    REQUIRE(overlap.most == 1);
}

TEST_CASE("A fixed delay counts from the end of the whole chain", "[periodic][fixed delay]")
{
    const auto pool = ThreadPoolExecutor::create(4);
    std::atomic<int> runs{0};
    std::atomic<std::int64_t> lastEnd{0};
    std::atomic<std::int64_t> shortestGap{std::numeric_limits<std::int64_t>::max()};
    auto now = [] {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };

    auto handle = Task::run(
                      pool.get(), 0ms, FixedDelay{20ms},
                      [&] {
        if (const auto end = lastEnd.load(); end != 0) {
            const auto gap = now() - end;
            auto shortest = shortestGap.load();
            while (gap < shortest && !shortestGap.compare_exchange_weak(shortest, gap)) {}
        }
    })
                      .then(pool.get(), [&] {
        std::this_thread::sleep_for(10ms); // the continuation is part of the run
        lastEnd = now();
        ++runs;
    }).submit();

    REQUIRE(waitFor([&runs] { return runs >= 4; }));
    handle.cancel();
    handle.wait();
    REQUIRE(shortestGap >= 19'000); // microseconds, with a little slack for the clock
}

TEST_CASE("A fixed-delay run that fails doesn't stop the series", "[periodic][fixed delay][exception]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> failures{0};

    auto handle = Task::run(executor.get(), 0ms, FixedDelay{1ms}, [] { throw std::runtime_error{"run failed"}; })
                      .onError([&failures](const std::exception&) {
        ++failures;
    }).submit();

    REQUIRE(waitFor([&] {
        executor->runPending();
        return failures >= 3;
    }));
    handle.cancel();
    REQUIRE(handle.waitFor(5s));
}

TEST_CASE("A fixed-delay series ends when it is canceled during a run", "[periodic][fixed delay][cancel]")
{
    const auto pool = ThreadPoolExecutor::create(2);
    std::atomic<int> runs{0};
    std::atomic<bool> inRun{false};
    TaskHandle<void> handle;

    handle = Task::run(pool.get(), 0ms, FixedDelay{1ms}, [&runs, &inRun] {
        inRun = true;
        std::this_thread::sleep_for(30ms);
        ++runs;
    }).submit();

    REQUIRE(waitFor([&inRun] { return inRun.load(); }));
    handle.cancel(); // while the entry is parked waiting for the run

    REQUIRE(handle.waitFor(5s));
    const int runsAtEnd = runs;
    std::this_thread::sleep_for(50ms);
    REQUIRE(runs <= runsAtEnd + 1);
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A fixed-delay series ends at once when it is canceled while a run waits in the queue", "[periodic][fixed delay][cancel]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> cancellations{0};

    auto handle = Task::run(executor.get(), 0ms, FixedDelay{1ms}, [] {})
                      .onError([&cancellations](const OperationCanceledException&) {
        ++cancellations;
    }).submit();

    REQUIRE(waitFor([&executor] { return executor->pendingCount() == 1; })); // the first run waits to be run
    handle.cancel();

    REQUIRE(handle.waitFor(5s)); // without running the executor
    executor->runPending();
    REQUIRE(cancellations == 1); // the queued run reports the cancellation
}

TEST_CASE("A fixed-delay series ends when its executor is destroyed", "[periodic][fixed delay][shutdown]")
{
    auto executor = ManualExecutor::create();
    auto handle = Task::run(executor.get(), 0ms, FixedDelay{1ms}, [] {}).submit();

    std::this_thread::sleep_for(10ms);
    executor.reset();

    REQUIRE(handle.waitFor(5s));
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A periodic task starts after its initial delay", "[periodic]")
{
    const auto executor = ManualExecutor::create();
    const auto start = std::chrono::steady_clock::now();
    std::atomic<std::int64_t> firstRun{0};

    auto handle = Task::run(executor.get(), 30ms, FixedDelay{1ms}, [&firstRun, start] {
        if (firstRun == 0) {
            firstRun = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        }
    }).submit();

    REQUIRE(waitFor([&] {
        executor->runPending();
        return firstRun != 0;
    }));
    handle.cancel();
    REQUIRE(firstRun >= 30);
}

namespace {
    // A task whose copies fail after a while: each periodic run is a copy.
    struct FailingCopies
    {
        std::shared_ptr<std::atomic<int>> copies = std::make_shared<std::atomic<int>>(0);
        std::shared_ptr<std::atomic<int>> runs = std::make_shared<std::atomic<int>>(0);

        FailingCopies() = default;
        FailingCopies(FailingCopies&&) noexcept = default;

        FailingCopies(const FailingCopies& other): copies{other.copies}, runs{other.runs}
        {
            if (++*copies % 2 == 0) {
                throw std::runtime_error{"copy failed"};
            }
        }

        void operator()() const
        {
            ++*runs;
        }
    };
} // namespace

TEST_CASE("A periodic run whose copy fails is skipped and reported; the series goes on", "[periodic][exception]")
{
    const vanilo::tests::UnhandledExceptions unhandled;
    const auto executor = ManualExecutor::create();
    FailingCopies task;
    const auto runs = task.runs;

    SECTION("fixed rate")
    {
        auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms}, std::move(task)).submit();
        REQUIRE(waitFor([&] {
            executor->runPending();
            return *runs >= 2 && unhandled.messages().size() >= 2;
        }));
        handle.cancel();
        handle.wait();
    }

    SECTION("fixed delay")
    {
        auto handle = Task::run(executor.get(), 0ms, FixedDelay{1ms}, std::move(task)).submit();
        REQUIRE(waitFor([&] {
            executor->runPending();
            return *runs >= 2 && unhandled.messages().size() >= 2;
        }));
        handle.cancel();
        handle.wait();
    }

    SECTION("fixed rate that skips")
    {
        // A copy that fails starts no run, so it must not count as a run that hasn't finished.
        auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Skip}, std::move(task)).submit();
        REQUIRE(waitFor([&] {
            executor->runPending();
            return *runs >= 2 && unhandled.messages().size() >= 2;
        }));
        handle.cancel();
        handle.wait();
    }

    REQUIRE(unhandled.messages().front() == "copy failed");
}

TEST_CASE("A fixed-rate series with a very short period skips the periods it can't keep up with", "[periodic][fixed rate]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> runs{0};

    auto handle = Task::run(executor.get(), 0ms, std::chrono::microseconds{1}, [&runs] { ++runs; }).submit();
    REQUIRE(waitFor([&] {
        executor->runPending();
        return runs >= 100;
    }));
    handle.cancel();
    REQUIRE(handle.waitFor(5s));
}

/// Fixed rate that skips
/// ================================================================================================

TEST_CASE("Overrun::Queue is what a fixed rate does by default", "[periodic][overrun]")
{
    const auto executor = ManualExecutor::create();
    auto task = [] {};
    TaskHandle<void> handle;

    SECTION("a plain duration")
    {
        handle = Task::run(executor.get(), 0ms, 1ms, task).submit();
    }

    SECTION("FixedRate without an overrun")
    {
        handle = Task::run(executor.get(), 0ms, FixedRate{1ms}, task).submit();
    }

    SECTION("Overrun::Queue")
    {
        handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Queue}, task).submit();
    }

    REQUIRE(waitFor([&executor] { return executor->pendingCount() >= 3; })); // the runs pile up
    handle.cancel();
    REQUIRE(handle.waitFor(5s));
}

TEST_CASE("A fixed rate that skips needs a positive interval", "[periodic][overrun]")
{
    const auto executor = ManualExecutor::create();

    REQUIRE_THROWS_AS(Task::run(executor.get(), 0ms, FixedRate{0ms, Overrun::Skip}, [] {}), std::invalid_argument);
}

TEST_CASE("A fixed rate that skips holds one run at most on an executor that doesn't keep up", "[periodic][overrun]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> runs{0};

    auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [&runs] { ++runs; }).submit();

    REQUIRE(waitFor([&executor] { return executor->pendingCount() == 1; }));
    std::this_thread::sleep_for(30ms); // many periods
    REQUIRE(executor->pendingCount() == 1);
    REQUIRE(runs == 0);

    executor->runPending(1);
    REQUIRE(runs == 1); // not a burst

    handle.cancel();
    REQUIRE(handle.waitFor(5s));
}

TEST_CASE("A fixed rate that skips goes on once the executor catches up", "[periodic][overrun]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> runs{0};

    auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [&runs] { ++runs; }).submit();

    REQUIRE(waitFor([&executor] { return executor->pendingCount() == 1; }));
    std::this_thread::sleep_for(20ms);

    REQUIRE(waitFor([&] {
        executor->runPending();
        return runs >= 3;
    }));
    handle.cancel();
    REQUIRE(handle.waitFor(5s));
}

TEST_CASE("Runs of a fixed rate that skips never overlap", "[periodic][overrun]")
{
    const auto pool = ThreadPoolExecutor::create(4);
    Overlap overlap;

    // Each run takes longer than the interval.
    auto handle = Task::run(pool.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [&overlap] { overlap.run(10ms); }).submit();

    REQUIRE(waitFor([&overlap] { return overlap.runs >= 5; }));
    handle.cancel();
    handle.wait();
    REQUIRE(overlap.most == 1);
}

TEST_CASE("A run of a fixed rate that skips lasts until the end of its whole chain", "[periodic][overrun]")
{
    const auto pool = ThreadPoolExecutor::create(2);
    const auto stalled = ManualExecutor::create();
    std::atomic<int> firstSteps{0};

    auto handle =
        Task::run(pool.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [&firstSteps] { ++firstSteps; }).then(stalled.get(), [] {}).submit();

    REQUIRE(waitFor([&stalled] { return stalled->pendingCount() == 1; }));
    std::this_thread::sleep_for(30ms); // many periods
    REQUIRE(stalled->pendingCount() == 1);
    REQUIRE(firstSteps == 1);

    stalled->runPending(1);
    REQUIRE(waitFor([&firstSteps] { return firstSteps >= 2; })); // that run has finished: the next one comes

    handle.cancel();
    REQUIRE(handle.waitFor(5s));
}

TEST_CASE("A failing run doesn't stop a fixed rate that skips", "[periodic][overrun][exception]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> failures{0};
    std::atomic<int> cancellations{0};

    SECTION("a run that throws")
    {
        auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [] { throw std::runtime_error{"run failed"}; })
                          .onError([&failures](const std::exception&) {
            ++failures;
        }).submit();

        REQUIRE(waitFor([&] {
            executor->runPending();
            return failures >= 3;
        }));
        handle.cancel();
        REQUIRE(handle.waitFor(5s));
    }

    SECTION("a run that cancels itself")
    {
        auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [] { throw OperationCanceledException{}; })
                          .onError([&cancellations](const OperationCanceledException&) {
            ++cancellations;
        }).submit();

        REQUIRE(waitFor([&] {
            executor->runPending();
            return cancellations >= 3;
        }));
        handle.cancel();
        REQUIRE(handle.waitFor(5s));
    }

    SECTION("a run that recovers")
    {
        auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Skip}, []() -> int { throw std::runtime_error{"run failed"}; })
                          .recover([&failures](const std::exception&) {
            ++failures;
            return 0;
        }).submit();

        REQUIRE(waitFor([&] {
            executor->runPending();
            return failures >= 3;
        }));
        handle.cancel();
        REQUIRE(handle.waitFor(5s));
    }
}

TEST_CASE("A fixed rate that skips ends when it is canceled", "[periodic][overrun][cancel]")
{
    SECTION("during a run")
    {
        const auto pool = ThreadPoolExecutor::create(2);
        std::atomic<bool> inRun{false};

        auto handle = Task::run(pool.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [&inRun] {
            inRun = true;
            std::this_thread::sleep_for(30ms);
        }).submit();

        REQUIRE(waitFor([&inRun] { return inRun.load(); }));
        handle.cancel();

        REQUIRE(handle.waitFor(5s));
        REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    }

    SECTION("while a run waits in the queue")
    {
        const auto executor = ManualExecutor::create();
        std::atomic<int> cancellations{0};

        auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [] {})
                          .onError([&cancellations](const OperationCanceledException&) {
            ++cancellations;
        }).submit();

        REQUIRE(waitFor([&executor] { return executor->pendingCount() == 1; }));
        handle.cancel();

        REQUIRE(handle.waitFor(5s)); // without running the executor
        executor->runPending();
        REQUIRE(cancellations == 1); // the queued run reports the cancellation
    }

    SECTION("through its token")
    {
        const auto executor = ManualExecutor::create();
        std::stop_source stop;

        auto handle = Task::run(executor.get(), stop.get_token(), 0ms, FixedRate{1ms, Overrun::Skip}, [] {}).submit();

        stop.request_stop();
        REQUIRE(handle.waitFor(5s));
        REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    }
}

TEST_CASE("A fixed rate that skips ends when its executor is destroyed", "[periodic][overrun][shutdown]")
{
    auto executor = ManualExecutor::create();
    auto handle = Task::run(executor.get(), 0ms, FixedRate{1ms, Overrun::Skip}, [] {}).submit();

    REQUIRE(waitFor([&executor] { return executor->pendingCount() == 1; }));
    executor.reset(); // cancels the queued run, which ends the skipping

    REQUIRE(handle.waitFor(5s));
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A fixed rate that skips keeps going when runs end as periods start", "[periodic][overrun][stress]")
{
    const auto pool = ThreadPoolExecutor::create(4);
    Overlap overlap;

    // The shortest period: most runs end around the moment the next period starts.
    auto handle =
        Task::run(pool.get(), 0ms, FixedRate{std::chrono::microseconds{1}, Overrun::Skip}, [&overlap] { overlap.run(0ms); }).submit();

    REQUIRE(waitFor([&overlap] { return overlap.runs >= 2000; }, 30s));
    handle.cancel();
    handle.wait();
    REQUIRE(overlap.most == 1);
}
