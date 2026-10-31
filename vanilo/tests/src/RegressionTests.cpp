#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/tasker/Scheduler.h>
#include <vanilo/tasker/Tasker.h>

#include "TestUtilities.h"

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;

// Regression tests for the bugs fixed in v1.0-b4. Each test describes the behaviour it protects.

using vanilo::tests::runsWithin;
using vanilo::tests::UnhandledExceptions;
using vanilo::tests::waitFor;

/// Exceptions in task chains
/// ================================================================================================

TEST_CASE("An exception skips the rest of the chain and reaches a later onException handler", "[regression][exception]")
{
    auto executor = LocalThreadExecutor::create();
    bool secondRan = false;
    std::string handled;

    Task::run(executor.get(), [] { throw std::runtime_error{"first failed"}; })
        .then(executor.get(), [&secondRan] { secondRan = true; })
        .onException([&handled](const std::exception& ex) { handled = ex.what(); });

    executor->process(10);

    REQUIRE_FALSE(secondRan);
    REQUIRE(handled == "first failed");
}

TEST_CASE("The nearest onException handler handles an exception, later handlers are not called", "[regression][exception]")
{
    auto executor = LocalThreadExecutor::create();
    int handledBy = 0;
    bool thirdRan = false;

    Task::run(executor.get(), [] { throw std::runtime_error{"failed"}; })
        .onException([&handledBy](const std::exception&) { handledBy = 1; })
        .then(executor.get(), [&thirdRan] { thirdRan = true; })
        .onException([&handledBy](const std::exception&) { handledBy = 2; });

    executor->process(10);

    REQUIRE(handledBy == 1);
    REQUIRE_FALSE(thirdRan);
}

TEST_CASE("A future at the end of a chain receives the exception even when a handler ran", "[regression][exception][future]")
{
    auto executor = LocalThreadExecutor::create();
    bool handled = false;

    auto future = Task::run(executor.get(), []() -> int { throw std::runtime_error{"failed"}; })
                      .onException([&handled](const std::exception&) { handled = true; })
                      .then(executor.get(), [](const int value) { return value * 2; })
                      .getFuture();

    executor->process(10);

    REQUIRE(handled);
    REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("Unhandled task exceptions are reported to the unhandled-exception handler", "[regression][exception]")
{
    const UnhandledExceptions unhandled;

    SECTION("on a LocalThreadExecutor")
    {
        auto executor = LocalThreadExecutor::create();
        Task::run(executor.get(), [] { throw std::runtime_error{"local"}; });
        executor->process(1);

        REQUIRE(unhandled.messages() == std::vector<std::string>{"local"});
    }

    SECTION("on a ThreadPoolExecutor")
    {
        const auto executor = ThreadPoolExecutor::create(1);
        Task::run(executor.get(), [] { throw std::runtime_error{"pool"}; });

        REQUIRE(waitFor([&unhandled] { return !unhandled.messages().empty(); }));
        REQUIRE(unhandled.messages() == std::vector<std::string>{"pool"});
    }

    SECTION("when an onException handler throws")
    {
        auto executor = LocalThreadExecutor::create();
        Task::run(executor.get(), [] { throw std::runtime_error{"task"}; }).onException([](const std::exception&) {
            throw std::logic_error{"handler"};
        });
        executor->process(10);

        REQUIRE(unhandled.messages() == std::vector<std::string>{"handler"});
    }

    SECTION("but not cancellations")
    {
        auto executor = LocalThreadExecutor::create();
        CancellationTokenSource source;
        source.cancel();
        Task::run(executor.get(), source.token(), [] {});
        executor->process(1);

        REQUIRE(unhandled.messages().empty());
    }
}

/// Executor shutdown
/// ================================================================================================

TEST_CASE("Destroying a LocalThreadExecutor cancels its pending tasks", "[regression][executor][local]")
{
    auto executor = LocalThreadExecutor::create();
    bool ran = false;
    bool canceled = false;

    auto future = Task::run(executor.get(), [] { return 1; }).getFuture();
    Task::run(executor.get(), [&ran] { ran = true; }).onException([&canceled](const std::exception& ex) {
        canceled = dynamic_cast<const OperationCanceledException*>(&ex) != nullptr;
    });

    executor.reset();

    REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
    REQUIRE_FALSE(ran);
    REQUIRE(canceled);
}

TEST_CASE("A task submitted to an executor that is shutting down is canceled", "[regression][executor]")
{
    const auto executor = LocalThreadExecutor::create();
    executor->getGuard()->terminate(); // what an executor's destructor does first

    auto future = Task::run(executor.get(), [] { return 1; }).getFuture();

    REQUIRE(executor->count() == 0);
    REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
}

TEST_CASE("Destroying a TaskScheduler cancels its pending scheduled tasks", "[regression][scheduler]")
{
    auto scheduler = TaskScheduler::create();
    const auto executor = LocalThreadExecutor::create();

    auto work = [] { return 42; };
    auto invocable = std::make_unique<internal::Invocable<decltype(work), int, void, true>>(executor.get(), std::move(work));
    auto future = invocable->getFuture();

    auto scheduled = ScheduledTask::create(scheduler.get(), std::chrono::steady_clock::now() + 1h);
    scheduled->setNext(std::move(invocable));
    scheduler->submit(std::move(scheduled));

    scheduler.reset();

    REQUIRE(executor->count() == 0);
    REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
}

TEST_CASE("A TaskScheduler can be created and destroyed repeatedly without hanging", "[regression][scheduler][stress]")
{
    // The destructor used to set the stop flag without holding the mutex, so the worker could miss
    // the wake-up and wait forever, hanging the destructor's join().
    REQUIRE(runsWithin(
        [] {
            for (int i = 0; i < 2000; ++i) {
                auto scheduler = TaskScheduler::create();
            }
        },
        60s));
}

/// Periodic tasks
/// ================================================================================================

TEST_CASE("A periodic task needs a positive interval", "[regression][periodic]")
{
    const auto executor = LocalThreadExecutor::create();

    REQUIRE_THROWS_AS(Task::run(executor.get(), CancellationToken{}, 0ms, 0ms, [] {}), std::invalid_argument);
    REQUIRE_THROWS_AS(Task::run(executor.get(), CancellationToken{}, 0ms, -1ms, [] {}), std::invalid_argument);
    REQUIRE(executor->count() == 0);
}

/// LifeCycleGuard
/// ================================================================================================

TEST_CASE("LifeCycleGuard never grants a lock that overlaps the end of terminate()", "[regression][LifeCycleGuard][stress]")
{
    // terminate() and tryIncrement() each write one variable and then read the other. With only
    // release/acquire ordering both could read the old value (store buffering), letting a caller
    // acquire the guard after terminate() had already returned.
    constexpr int Rounds = 200;
    constexpr int Threads = 4;

    for (int round = 0; round < Rounds; ++round) {
        LifeCycleGuard guard;
        std::atomic<bool> start{false};
        std::atomic<bool> terminated{false};
        std::atomic<int> violations{0};
        std::vector<std::thread> threads;

        for (int i = 0; i < Threads; ++i) {
            threads.emplace_back([&] {
                while (!start.load()) {
                    std::this_thread::yield();
                }
                for (int attempt = 0; attempt < 1000; ++attempt) {
                    if (const auto lock = guard.acquire()) {
                        if (terminated.load()) {
                            violations.fetch_add(1);
                        }
                    }
                }
            });
        }

        start = true;
        guard.terminate();
        terminated = true;

        for (auto& thread : threads) {
            thread.join();
        }
        REQUIRE(violations == 0);
    }
}

/// CancellationToken subscriptions
/// ================================================================================================

TEST_CASE("unsubscribe() waits for a callback running on another thread", "[regression][token][subscription]")
{
    CancellationTokenSource source;
    std::atomic<bool> callbackStarted{false};
    std::atomic<bool> releaseCallback{false};
    std::atomic<bool> callbackFinished{false};

    auto subscription = source.token().subscribe([&] {
        callbackStarted = true;
        while (!releaseCallback) {
            std::this_thread::yield();
        }
        callbackFinished = true;
    });

    std::thread canceller{[&source] { source.cancel(); }};
    REQUIRE(waitFor([&callbackStarted] { return callbackStarted.load(); }));

    std::atomic<bool> unsubscribed{false};
    std::thread unsubscriber{[&subscription, &unsubscribed] {
        subscription.unsubscribe();
        unsubscribed = true;
    }};

    std::this_thread::sleep_for(50ms);
    const bool returnedEarly = unsubscribed.load();

    releaseCallback = true;
    unsubscriber.join();
    canceller.join();

    REQUIRE_FALSE(returnedEarly);
    REQUIRE(callbackFinished);
}

TEST_CASE("A callback can unsubscribe itself without deadlocking", "[regression][token][subscription]")
{
    CancellationTokenSource source;
    std::optional<CancellationToken::Subscription> subscription;
    bool called = false;

    subscription = source.token().subscribe([&] {
        called = true;
        subscription->unsubscribe();
    });

    REQUIRE(runsWithin([&source] { source.cancel(); }, 5s));
    REQUIRE(called);
}

/// Callables passed by name
/// ================================================================================================

namespace {
    int answer()
    {
        return 42;
    }
} // namespace

TEST_CASE("Named callables (lvalues) can be used as tasks and handlers", "[regression][binder]")
{
    // FunctionTraits used to be applied to the deduced reference type, so only temporaries compiled.
    const auto executor = LocalThreadExecutor::create();

    auto lambda = [] { return 1; };
    const auto constLambda = [](const int value) { return value + 1; };
    auto* functionPointer = &answer;
    std::function<int(int)> function = [](const int value) { return value * 2; };
    bool handled = false;
    auto handler = [&handled](const std::exception&) { handled = true; };

    auto chained = Task::run(executor.get(), lambda).then(executor.get(), constLambda).then(executor.get(), function).getFuture();
    auto fromPointer = Task::run(executor.get(), functionPointer).getFuture();
    Task::run(executor.get(), [] { throw std::runtime_error{"failed"}; }).onException(handler);

    executor->process(10);

    REQUIRE(chained.get() == 4);
    REQUIRE(fromPointer.get() == 42);
    REQUIRE(handled);
}
