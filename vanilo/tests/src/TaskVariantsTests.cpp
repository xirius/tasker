#include <vanilo/tasker/Scheduler.h>
#include <vanilo/tasker/Tasker.h>

#include "TestUtilities.h"

#include <catch2/catch_all.hpp>

#include <atomic>
#include <future>
#include <stdexcept>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::RecordingTask;
using vanilo::tests::UnhandledExceptions;
using vanilo::tests::waitFor;

/// Task shapes not covered elsewhere
/// ================================================================================================

TEST_CASE("A task returning a value doesn't need a continuation or a future", "[task]")
{
    const auto executor = LocalThreadExecutor::create();
    bool ran = false;

    Task::run(executor.get(), [&ran] {
        ran = true;
        return 42;
    });
    executor->process(1);

    REQUIRE(ran);
}

TEST_CASE("Task::run with a zero delay runs without the scheduler", "[task][delay]")
{
    const auto executor = LocalThreadExecutor::create();
    bool ran = false;

    Task::run(executor.get(), CancellationToken{}, 0ms, [&ran] { ran = true; });

    REQUIRE(executor->count() == 1); // queued at once, not waiting in the scheduler
    executor->process(1);
    REQUIRE(ran);
}

TEST_CASE("A periodic chain passes each run's result to its continuation", "[periodic][chain]")
{
    const auto executor = LocalThreadExecutor::create();
    CancellationTokenSource source;
    std::atomic<int> runs{0};
    std::atomic<int> sum{0};

    Task::run(executor.get(), source.token(), 0ms, 5ms, [&runs] { return ++runs; }).then(executor.get(), [&sum](const int run) {
        sum += run;
    });

    REQUIRE(waitFor([&] {
        executor->process(10);
        return runs >= 3 && sum >= 6; // 1 + 2 + 3
    }));
    source.cancel();
}

/// TaskScheduler internals
/// ================================================================================================

TEST_CASE("TaskScheduler::count() reports the pending scheduled tasks", "[scheduler]")
{
    const auto scheduler = TaskScheduler::create();
    const auto executor = LocalThreadExecutor::create();

    auto first = ScheduledTask::create(scheduler.get(), std::chrono::steady_clock::now() + 1h);
    auto second = ScheduledTask::create(scheduler.get(), std::chrono::steady_clock::now() + 2h);
    scheduler->submit(std::move(first));
    scheduler->submit(std::move(second));

    REQUIRE(scheduler->count() == 2);
}

TEST_CASE("TaskScheduler runs a chainable task submitted without a schedule at once", "[scheduler]")
{
    const auto scheduler = TaskScheduler::create();
    const auto executor = LocalThreadExecutor::create();
    std::atomic<bool> ran{false};

    auto work = [&ran] { ran = true; };
    scheduler->submit(std::make_unique<internal::Invocable<decltype(work), void, void>>(executor.get(), std::move(work)));

    // The scheduler forwards it to its own executor right away.
    REQUIRE(waitFor([&] {
        executor->process(1);
        return ran.load();
    }));
}

TEST_CASE("TaskScheduler reports a plain task that throws", "[scheduler][exception]")
{
    struct ThrowingTask final: Task
    {
        void cancel() noexcept override
        {
            // Nothing to cancel
        }

        void run() override
        {
            throw std::runtime_error{"plain task failed"};
        }
    };

    const UnhandledExceptions unhandled;
    const auto scheduler = TaskScheduler::create();
    scheduler->submit(std::make_unique<ThrowingTask>());

    REQUIRE(waitFor([&unhandled] { return !unhandled.messages().empty(); }));
    REQUIRE(unhandled.messages() == std::vector<std::string>{"plain task failed"});
}

TEST_CASE("TaskScheduler runs a plain task", "[scheduler][adapter]")
{
    const auto scheduler = TaskScheduler::create();
    std::atomic<int> runs{0};
    std::atomic<bool> canceled{false};

    scheduler->submit(std::make_unique<RecordingTask>(runs, canceled));

    REQUIRE(waitFor([&runs] { return runs == 1; }));
    REQUIRE_FALSE(canceled);
}

TEST_CASE("ScheduledTask::clone keeps the schedule and the token", "[scheduler][clone]")
{
    const auto scheduler = TaskScheduler::create();
    const CancellationTokenSource source;
    const auto due = std::chrono::steady_clock::now() + 1h;

    auto task = ScheduledTask::create(scheduler.get(), due, std::chrono::seconds{5});
    task->setToken(source.token());

    const auto cloned = task->clone();
    const auto* clonedTask = dynamic_cast<ScheduledTask*>(cloned.get());

    REQUIRE(clonedTask != nullptr);
    REQUIRE(clonedTask->due() == due);
    REQUIRE(clonedTask->isPeriodic());
    REQUIRE(clonedTask->interval() == std::chrono::seconds{5});
    REQUIRE(clonedTask->getToken() == source.token());
    REQUIRE(clonedTask->sequence() != task->sequence());
}

TEST_CASE("A periodic ScheduledTask does nothing when canceled or empty", "[scheduler][periodic]")
{
    const auto scheduler = TaskScheduler::create();
    const auto executor = LocalThreadExecutor::create();
    std::atomic<int> runs{0};

    auto makeChain = [&executor, &runs] {
        auto work = [&runs] { ++runs; };
        return std::make_unique<internal::Invocable<decltype(work), void, void>>(executor.get(), std::move(work));
    };

    SECTION("canceled token")
    {
        CancellationTokenSource source;
        auto task = ScheduledTask::create(scheduler.get(), std::chrono::steady_clock::now(), std::chrono::seconds{1});
        task->setToken(source.token());
        task->setNext(makeChain());
        source.cancel();

        task->run();
    }

    SECTION("canceled task")
    {
        auto task = ScheduledTask::create(scheduler.get(), std::chrono::steady_clock::now(), std::chrono::seconds{1});
        task->setNext(makeChain());
        task->cancel();

        task->run();
    }

    SECTION("nothing to run")
    {
        auto task = ScheduledTask::create(scheduler.get(), std::chrono::steady_clock::now(), std::chrono::seconds{1});
        task->run();
    }

    executor->process(10);
    REQUIRE(runs == 0);
}
