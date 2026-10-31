#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using namespace vanilo::tasker;
using namespace vanilo::concurrent;

// The shapes of a single task: with or without an argument and a result, with or without a handle.

TEST_CASE("Run task: Result<Arg> without taking the result", "[task]")
{
    const auto executor = ManualExecutor::create();
    double result = 0;

    Task::run(executor.get(), [] { return 1.23; }).then(executor.get(), [&result](const double value) {
        return result = 3.21 * value;
    }).submit();
    executor->runPending();

    REQUIRE(result == 1.23 * 3.21);
}

TEST_CASE("Run task: void<void> and take a TaskHandle<void>", "[task]")
{
    const auto executor = ManualExecutor::create();
    auto value = 0;

    auto handle = Task::run(executor.get(), [&value]() { value = 1; }).submit();
    STATIC_REQUIRE(std::is_same_v<decltype(handle), TaskHandle<void>>);

    executor->runPending();
    handle.get();

    REQUIRE(value == 1);
}

TEST_CASE("Run task: void<Arg> and take a TaskHandle<void>", "[task]")
{
    const auto executor = ManualExecutor::create();
    auto value = 0;

    auto handle = Task::run(executor.get(), [] { return 1; }).then(executor.get(), [&value](const int val) { value = val; }).submit();

    executor->runPending();
    handle.get();

    REQUIRE(value == 1);
}

TEST_CASE("Run task: Result<void> and take a TaskHandle<Result>", "[task]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), [] { return 123; }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 123);
}

TEST_CASE("Run task: Result<Arg> and take a TaskHandle<Result>", "[task]")
{
    const auto executor = ManualExecutor::create();

    auto handle =
        Task::run(executor.get(), [] { return 1.23; }).then(executor.get(), [](const double value) { return value * 3.21; }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 1.23 * 3.21);
}

TEST_CASE("Run task: void<void> and take a TaskHandle<void> with exception", "[task]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), [] { throw std::invalid_argument{"Something happened"}; }).submit();
    executor->runPending();

    CHECK_THROWS_AS(handle.get(), std::invalid_argument);
}

TEST_CASE("Run task: void<void> chained and take a TaskHandle<void> with exception", "[task]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), [] { throw std::invalid_argument{"Something happened"}; })
                      .then(executor.get(), [] { /* Ignore */ })
                      .submit();
    executor->runPending();

    CHECK_THROWS_AS(handle.get(), std::invalid_argument);
}

TEST_CASE("Run task: void<void> runs before it throws", "[task]")
{
    const auto executor = ManualExecutor::create();
    auto value = 0;

    auto handle = Task::run(executor.get(), [&value] {
        value = 1;
        throw std::invalid_argument{"Something happened"};
    }).submit();
    executor->runPending();

    CHECK_THROWS_AS(handle.get(), std::invalid_argument);
    REQUIRE(value == 1);
}

TEST_CASE("Run task: void<Arg> runs before it throws", "[task]")
{
    const auto executor = ManualExecutor::create();
    auto value = 0;

    auto handle = Task::run(executor.get(), [] { return 123; })
                      .then(executor.get(), [&value](const int val) {
        value = val;
        throw std::invalid_argument{"Something happened"};
    }).submit();
    executor->runPending();

    CHECK_THROWS_AS(handle.get(), std::invalid_argument);
    REQUIRE(value == 123);
}

TEST_CASE("Run task: Result<void> with a handle taken later in the chain", "[task]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), [] { return 123; }).then(executor.get(), [](const int value) { return value; }).submit();
    executor->runPending(1);
    REQUIRE_FALSE(handle.isReady()); // the second task is still queued

    executor->runPending(1);
    REQUIRE(handle.get() == 123);
}

TEST_CASE("Run task: Result<Arg> with the argument passed through a tuple", "[task]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), [] { return std::tuple{1.23, 2}; })
                      .then(executor.get(), [](const double value, const int factor) {
        return value * factor;
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 1.23 * 2);
}

TEST_CASE("Periodic task executes multiple times with chain", "[task][periodic]")
{
    const auto executor = ThreadPoolExecutor::create(2);
    std::stop_source stopSource;
    std::atomic_int counter{0};

    Task::run(executor.get(), stopSource.get_token(), 10ms, 30ms, [] { return 1; })
        .then(executor.get(), [&counter](const int val) {
        counter += val;
    }).submit();

    std::this_thread::sleep_for(150ms);
    stopSource.request_stop();

    // At 10ms, 40ms, 70ms, 100ms, 130ms. Should be ~5 executions.
    const int finalCount = counter.load();
    CHECK(finalCount >= 3);
    CHECK(finalCount <= 8);
}

TEST_CASE("Periodic task is canceled by its stop token", "[task][periodic][cancel]")
{
    const auto executor = ThreadPoolExecutor::create(2);
    std::stop_source stopSource;
    std::atomic_int counter{0};

    auto handle = Task::run(executor.get(), stopSource.get_token(), 10ms, 20ms, [&counter] { ++counter; }).submit();

    std::this_thread::sleep_for(50ms);
    // Should have run ~3 times (10, 30, 50)
    const int countBefore = counter.load();
    REQUIRE(countBefore >= 1);

    stopSource.request_stop();
    REQUIRE(handle.waitFor(5s)); // the series ends at once, not at its next run
    std::this_thread::sleep_for(100ms);

    const int countAfter = counter.load();
    // It should not have run anymore after cancellation; give some slack for a run already in flight
    CHECK(countAfter <= countBefore + 1);
}

TEST_CASE("A periodic task's handle reports the end of the series", "[task][periodic]")
{
    const auto executor = ThreadPoolExecutor::create(1);
    auto handle = Task::run(executor.get(), 1ms, 1ms, [] { /* Ignore */ }).submit();
    STATIC_REQUIRE(std::is_same_v<decltype(handle), TaskHandle<void>>);

    std::this_thread::sleep_for(20ms);
    REQUIRE_FALSE(handle.isReady());

    handle.cancel();
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A handle throws when the executor is destroyed before the task runs", "[executor][handle]")
{
    auto executor = ThreadPoolExecutor::create(1);
    auto handle = Task::run(executor.get(), 100ms, [] { return 123; }).submit();

    // Destroy the executor before the delay has passed
    executor.reset();

    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}
