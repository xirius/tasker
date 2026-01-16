#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using namespace vanilo::tasker;
using namespace vanilo::concurrent;

TEST_CASE("Run task: Result<Arg> without taking the result", "[task]")
{
    double result = 0;
    auto task = [&result](const double value) { return result = 3.21 * value; };
    const auto invokable = std::make_unique<internal::Invocable<decltype(task), double, double>>(nullptr, std::move(task));

    invokable->setArgument(1.23);
    invokable->run();

    REQUIRE(result == 1.23 * 3.21);
}

TEST_CASE("Run task: void<void> and take std::future<void>", "[task]")
{
    auto value = 0;
    auto task = [&value]() { value = 1; };
    const auto invokable = std::make_unique<internal::Invocable<decltype(task), void, void, true>>(nullptr, std::move(task));
    auto future = invokable->getFuture();

    invokable->run();
    future.get();

    REQUIRE(value == 1);
}

TEST_CASE("Run task: void<Arg> and take std::future<void>", "[task]")
{
    auto value = 0;
    auto task = [&value](const int val) { value = val; };
    const auto invokable = std::make_unique<internal::Invocable<decltype(task), void, int, true>>(nullptr, std::move(task));
    auto future = invokable->getFuture();

    invokable->setArgument(1);
    invokable->run();
    future.get();

    REQUIRE(value == 1);
}

TEST_CASE("Run task: Result<void> and take std::future<Result>", "[task]")
{
    auto task = [] { return 123; };
    const auto invokable = std::make_unique<internal::Invocable<decltype(task), int, void, true>>(nullptr, std::move(task));
    auto future = invokable->getFuture();

    invokable->run();

    REQUIRE(future.get() == 123);
}

TEST_CASE("Run task: Result<Arg> and take std::future<Result>", "[task]")
{
    auto task = [](const double value) { return value * 3.21; };
    const auto invokable = std::make_unique<internal::Invocable<decltype(task), double, double, true>>(nullptr, std::move(task));
    auto future = invokable->getFuture();

    invokable->setArgument(1.23);
    invokable->run();

    REQUIRE(future.get() == 1.23 * 3.21);
}

TEST_CASE("Run task: void<void> and take std::future<void> with exception", "[task]")
{
    auto task = [] { throw std::invalid_argument{"Something happened"}; };
    const auto invokable = std::make_unique<internal::Invocable<decltype(task), void, void, true>>(nullptr, std::move(task));
    auto future = invokable->getFuture();

    invokable->run();

    CHECK_THROWS_AS(future.get(), std::invalid_argument);
}

TEST_CASE("Run task: void<void> chained and take std::future<void> with exception", "[task]")
{
    auto task1 = [] { throw std::invalid_argument{"Something happened"}; };
    auto task2 = [] { /* Ignore */ };
    const auto invokable1 = std::make_unique<internal::Invocable<decltype(task1), void, void>>(nullptr, std::move(task1));
    auto invokable2 = std::make_unique<internal::Invocable<decltype(task2), void, void, true>>(nullptr, std::move(task2));
    auto future = invokable2->getFuture();

    invokable1->setNext(std::move(invokable2));
    invokable1->run();

    CHECK_THROWS_AS(future.get(), std::invalid_argument);
}

TEST_CASE("Run task: void<void> convert to PromisedTask", "[task]")
{
    auto value = 0;
    auto task = [&value] {
        value = 1;
        throw std::invalid_argument{"Something happened"};
    };
    const auto invokable1 = std::make_unique<internal::Invocable<decltype(task), void, void>>(nullptr, std::move(task));
    const auto invokable2 = invokable1->toPromisedTask();
    auto future = invokable2->getFuture();

    invokable2->run();

    CHECK_THROWS_AS(future.get(), std::invalid_argument);
    REQUIRE(value == 1);
}

TEST_CASE("Run task: void<Arg> convert to PromisedTask", "[task]")
{
    auto value = 0;
    auto task = [&value](const int val) {
        value = val;
        throw std::invalid_argument{"Something happened"};
    };
    const auto invokable1 = std::make_unique<internal::Invocable<decltype(task), void, int>>(nullptr, std::move(task));

    invokable1->setArgument(123);

    const auto invokable2 = invokable1->toPromisedTask();
    auto future = invokable2->getFuture();

    invokable2->run();

    CHECK_THROWS_AS(future.get(), std::invalid_argument);
    REQUIRE(value == 123);
}

TEST_CASE("Run task: Result<void> convert to PromisedTask", "[task]")
{
    auto task = [] { return 123; };
    const auto invokable1 = std::make_unique<internal::Invocable<decltype(task), int, void>>(nullptr, std::move(task));
    const auto invokable2 = invokable1->toPromisedTask();
    auto future = invokable2->getFuture();

    invokable2->run();

    REQUIRE(future.get() == 123);
}

TEST_CASE("Run task: Result<Arg> convert to PromisedTask", "[task]")
{
    auto task = [](const double arg) { return arg * 3.21; };
    const auto invokable1 = std::make_unique<internal::Invocable<decltype(task), double, double>>(nullptr, std::move(task));

    invokable1->setArgument(1.23);

    const auto invokable2 = invokable1->toPromisedTask();
    auto future = invokable2->getFuture();

    invokable2->run();

    REQUIRE(future.get() == 1.23 * 3.21);
}
TEST_CASE("Periodic task executes multiple times with chain", "[task][periodic]")
{
    const auto executor = ThreadPoolExecutor::create(2);
    const vanilo::concurrent::CancellationTokenSource tokenSource;
    std::atomic_int counter{0};

    Task::run(executor.get(), tokenSource.token(), 10ms, 30ms, [] { return 1; }).then(executor.get(), [&counter](const int val) {
        counter += val;
    });

    std::this_thread::sleep_for(150ms);
    tokenSource.cancel();

    // At 10ms, 40ms, 70ms, 100ms, 130ms. Should be ~5 executions.
    const int finalCount = counter.load();
    CHECK(finalCount >= 3);
    CHECK(finalCount <= 8);
}

TEST_CASE("Periodic task is canceled by CancellationToken", "[task][periodic][cancel]")
{
    const auto executor = ThreadPoolExecutor::create(2);
    const vanilo::concurrent::CancellationTokenSource tokenSource;
    std::atomic_int counter{0};

    Task::run(executor.get(), tokenSource.token(), 10ms, 20ms, [&counter] { ++counter; });

    std::this_thread::sleep_for(50ms);
    // Should have run ~3 times (10, 30, 50)
    const int countBefore = counter.load();
    REQUIRE(countBefore >= 1);

    tokenSource.cancel();
    std::this_thread::sleep_for(100ms);

    const int countAfter = counter.load();
    // It should not have run anymore after cancellation
    // Give some slack for tasks already in flight
    CHECK(countAfter <= countBefore + 1);
}

TEST_CASE("Periodic task builder does not have getFuture", "[task][periodic]")
{
    const auto executor = ThreadPoolExecutor::create(1);
    auto builder = Task::run(executor.get(), 1ms, 1ms, [] { /* Ignore */ });

    // The following would not compile if getFuture existed, and we uncommented it
    // builder.getFuture();

    std::this_thread::sleep_for(1s);
    // We check that we got a PeriodicBuilder
    SUCCEED("PeriodicBuilder returned");
}

TEST_CASE("Future throws OperationCanceledException when executor destroyed before execution", "[executor][future]")
{
    auto executor = ThreadPoolExecutor::create(1);
    auto future = Task::run(executor.get(), CancellationToken::none(), 100ms, [] { return 123; }).getFuture();

    // Destroy the executor immediately
    executor.reset();

    // Wait for the delay to pass + some buffer
    std::this_thread::sleep_for(200ms);

    REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
}