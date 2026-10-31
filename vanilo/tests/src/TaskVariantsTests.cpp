#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <stdexcept>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::UnhandledExceptions;
using vanilo::tests::waitFor;

/// Task shapes not covered elsewhere
/// ================================================================================================

TEST_CASE("A task returning a value doesn't need a continuation or a handle", "[task]")
{
    const auto executor = ManualExecutor::create();
    bool ran = false;

    Task::run(executor.get(), [&ran] {
        ran = true;
        return 42;
    }).submit();
    executor->runPending(1);

    REQUIRE(ran);
}

TEST_CASE("Task::run with a zero delay doesn't go through the timer queue", "[task][delay]")
{
    const auto executor = ManualExecutor::create();
    bool ran = false;

    Task::run(executor.get(), 0ms, [&ran] { ran = true; }).submit();

    REQUIRE(executor->pendingCount() == 1); // queued at once, not waiting for the timer
    executor->runPending(1);
    REQUIRE(ran);
}

TEST_CASE("A delayed task runs after its delay", "[task][delay]")
{
    const auto executor = ManualExecutor::create();
    const auto start = std::chrono::steady_clock::now();

    auto handle = Task::run(executor.get(), 30ms, [start] { return std::chrono::steady_clock::now() - start; }).submit();

    REQUIRE(waitFor([&] {
        executor->runPending();
        return handle.isReady();
    }));
    REQUIRE(handle.get() >= 30ms);
}

TEST_CASE("A periodic chain passes each run's result to its continuation", "[periodic][chain]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    std::atomic<int> runs{0};
    std::atomic<int> sum{0};

    Task::run(executor.get(), source.get_token(), 0ms, 5ms, [&runs] { return ++runs; })
        .then(executor.get(), [&sum](const int run) {
        sum += run;
    }).submit();

    REQUIRE(waitFor([&] {
        executor->runPending(10);
        return runs >= 3 && sum >= 6; // 1 + 2 + 3
    }));
    source.request_stop();
}

TEST_CASE("Each run of a periodic task is a fresh copy of the task", "[periodic]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> runs{0};
    std::atomic<int> maxCalls{0};

    // The counter captured by value starts at 0 in every run.
    auto handle = Task::run(executor.get(), 0ms, 2ms, [&runs, &maxCalls, calls = 0]() mutable {
        maxCalls = std::max(maxCalls.load(), ++calls);
        ++runs;
    }).submit();

    REQUIRE(waitFor([&] {
        executor->runPending();
        return runs >= 3;
    }));
    handle.cancel();
    REQUIRE(maxCalls == 1);
}

TEST_CASE("A periodic run's error goes to its handler, or is reported, and the series goes on", "[periodic][exception]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> runs{0};

    SECTION("with an onError handler")
    {
        std::atomic<int> handled{0};
        auto handle = Task::run(executor.get(), 0ms, 2ms, [&runs] {
            ++runs;
            throw std::runtime_error{"run failed"};
        }).onError([&handled](const std::exception&) {
            ++handled;
        }).submit();

        REQUIRE(waitFor([&] {
            executor->runPending();
            return handled >= 2;
        }));
        handle.cancel();
    }

    SECTION("without a handler")
    {
        const UnhandledExceptions unhandled;
        auto handle = Task::run(executor.get(), 0ms, 2ms, [&runs] {
            ++runs;
            throw std::runtime_error{"run failed"};
        }).submit();

        REQUIRE(waitFor([&] {
            executor->runPending();
            return unhandled.messages().size() >= 2;
        }));
        handle.cancel();
        handle.wait();
    }

    REQUIRE(runs >= 2);
}

TEST_CASE("A periodic series ends when its executor is destroyed", "[periodic][shutdown]")
{
    auto executor = ManualExecutor::create();
    auto handle = Task::run(executor.get(), 0ms, 2ms, [] {}).submit();

    executor.reset();

    REQUIRE(handle.waitFor(5s));
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A canceled handle ends the series; a handle that is dropped doesn't", "[periodic][handle]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> runs{0};
    std::stop_source source;

    {
        auto dropped = Task::run(executor.get(), source.get_token(), 0ms, 2ms, [&runs] { ++runs; }).submit();
    }

    REQUIRE(waitFor([&] {
        executor->runPending();
        return runs >= 3;
    }));
    source.request_stop();
}
