#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

// Cancellation goes through std::stop_source and std::stop_token: what tasker does with them.

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::runsWithin;
using vanilo::tests::waitFor;

TEST_CASE("A default std::stop_token never stops a chain", "[token]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), std::stop_token{}, [](const std::stop_token& token) { return token.stop_possible(); }).submit();
    executor->runPending();

    REQUIRE(handle.get()); // the chain's own token: it can still be stopped through the handle
}

TEST_CASE("A chain whose token is already stopped doesn't run", "[token][cancel]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    source.request_stop();
    bool ran = false;

    auto handle = Task::run(executor.get(), source.get_token(), [&ran] { ran = true; }).submit();
    executor->runPending();

    REQUIRE_FALSE(ran);
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A task's token sees a stop request through the user's token", "[token]")
{
    const auto executor = ThreadPoolExecutor::create(1);
    std::stop_source source;
    std::atomic<bool> started{false};

    auto handle = Task::run(executor.get(), source.get_token(), [&started](const std::stop_token& token) {
        started = true;
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        throwIfStopRequested(token);
    }).submit();

    REQUIRE(waitFor([&started] { return started.load(); }));
    source.request_stop();

    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A task's token sees a stop request through the handle", "[token][handle]")
{
    const auto executor = ThreadPoolExecutor::create(1);
    std::atomic<bool> started{false};

    auto handle = Task::run(executor.get(), [&started](const std::stop_token& token) {
        started = true;
        while (!token.stop_requested()) {
            std::this_thread::yield();
        }
        return 42; // a task may also finish normally after a stop request
    }).submit();

    REQUIRE(waitFor([&started] { return started.load(); }));
    handle.cancel();

    REQUIRE(handle.get() == 42);
}

TEST_CASE("The tasks of a chain receive the same token", "[token]")
{
    const auto executor = ManualExecutor::create();
    std::stop_token first;
    std::stop_token second;

    auto handle = Task::run(executor.get(), [&first](const std::stop_token& token) { first = token; })
                      .then(executor.get(), [&second](const std::stop_token& token) {
        second = token;
    }).submit();
    executor->runPending();

    REQUIRE(first.stop_possible()); // the handle keeps the chain's stop source
    REQUIRE(first == second);
}

TEST_CASE("Canceling a finished chain changes nothing", "[token][handle]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), [] { return 1; }).submit();
    executor->runPending();
    handle.cancel();

    REQUIRE(handle.get() == 1);
}

TEST_CASE("Canceling an empty handle does nothing", "[token][handle]")
{
    const TaskHandle<int> handle;

    REQUIRE_FALSE(handle.valid());
    REQUIRE_FALSE(handle.isReady());
    REQUIRE_NOTHROW(handle.cancel());
    REQUIRE_THROWS_AS(handle.wait(), std::future_error);
}

TEST_CASE("throwIfStopRequested throws only after a stop request", "[token]")
{
    std::stop_source source;

    REQUIRE_NOTHROW(throwIfStopRequested(source.get_token()));
    REQUIRE_NOTHROW(throwIfStopRequested(std::stop_token{}));

    source.request_stop();
    REQUIRE_THROWS_AS(throwIfStopRequested(source.get_token()), OperationCanceledException);
}

TEST_CASE("A stop request wakes ManualExecutor::runUntil()", "[token][executor][manual]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;

    std::thread stopper{[&source] {
        std::this_thread::sleep_for(20ms);
        source.request_stop();
    }};

    REQUIRE(runsWithin([&] { executor->runUntil(source.get_token()); }, 5s));
    stopper.join();
}

TEST_CASE("A delayed chain is canceled as soon as its token is stopped", "[token][delay]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;

    auto handle = Task::run(executor.get(), source.get_token(), 1h, [] { return 1; }).submit();
    source.request_stop();

    REQUIRE(handle.waitFor(5s)); // it doesn't wait for its hour
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A delayed chain is canceled as soon as its handle is canceled", "[token][delay][handle]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), 1h, [] { return 1; }).submit();
    handle.cancel();

    REQUIRE(handle.waitFor(5s));
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A posted function doesn't run once its token is stopped", "[token][post]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    int runs = 0;

    Task::post(executor.get(), source.get_token(), [&runs] { ++runs; });
    Task::post(executor.get(), source.get_token(), [&runs] { ++runs; });
    executor->runPending(1);
    source.request_stop();
    executor->runPending();

    REQUIRE(runs == 1);
}

TEST_CASE("A posted function receives the token it was posted with", "[token][post]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    std::stop_token received;

    Task::post(executor.get(), source.get_token(), [&received](const std::stop_token& token) { received = token; });
    executor->runPending();

    REQUIRE(received == source.get_token());
}

TEST_CASE("A chain has exactly one outcome when it is canceled while it finishes", "[token][handle][stress]")
{
    const auto executor = ThreadPoolExecutor::create(4);

    for (int i = 0; i < 2000; ++i) {
        auto handle = Task::run(executor.get(), [] { return 1; }).then(executor.get(), [](const int value) { return value + 1; }).submit();
        std::thread canceler{[&handle] { handle.cancel(); }};

        int result = 0;
        try {
            handle.wait();
            canceler.join();
            result = handle.get();
        }
        catch (const OperationCanceledException&) {
            result = -1; // canceled before the second task: fine too
        }
        REQUIRE((result == 2 || result == -1));
    }
}

TEST_CASE("std::stop_callback waits for a callback running on another thread", "[regression][token]")
{
    // Tasker relies on this: a delayed chain's wake-up callback is unregistered while it may be running.
    std::stop_source source;
    std::atomic<bool> callbackStarted{false};
    std::atomic<bool> releaseCallback{false};
    std::atomic<bool> callbackFinished{false};

    auto callback = std::make_unique<std::stop_callback<std::function<void()>>>(source.get_token(), [&] {
        callbackStarted = true;
        while (!releaseCallback) {
            std::this_thread::yield();
        }
        callbackFinished = true;
    });

    std::thread stopper{[&source] { source.request_stop(); }};
    REQUIRE(waitFor([&callbackStarted] { return callbackStarted.load(); }));

    std::atomic<bool> unregistered{false};
    std::thread unregisterer{[&callback, &unregistered] {
        callback.reset();
        unregistered = true;
    }};

    std::this_thread::sleep_for(50ms);
    const bool returnedEarly = unregistered.load();

    releaseCallback = true;
    unregisterer.join();
    stopper.join();

    REQUIRE_FALSE(returnedEarly);
    REQUIRE(callbackFinished);
}

TEST_CASE("A std::stop_callback can be destroyed by its own callback", "[regression][token]")
{
    std::stop_source source;
    std::optional<std::stop_callback<std::function<void()>>> callback;
    bool called = false;

    callback.emplace(source.get_token(), [&] {
        called = true;
        callback.reset();
    });

    REQUIRE(runsWithin([&source] { source.request_stop(); }, 5s));
    REQUIRE(called);
}
