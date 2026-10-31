#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::StandardErrorCapture;
using vanilo::tests::UnhandledExceptions;
using vanilo::tests::waitFor;

/// Where onError handlers run
/// ================================================================================================

TEST_CASE("onError with an executor runs the handler on that executor", "[exception][executor]")
{
    const auto taskExecutor = ManualExecutor::create();
    const auto handlerExecutor = ManualExecutor::create();
    bool handled = false;

    Task::run(taskExecutor.get(), [] { throw std::runtime_error{"failed"}; })
        .onError(handlerExecutor.get(), [&handled](const std::exception&) {
        handled = true;
    }).submit();

    taskExecutor->runPending(1);
    REQUIRE_FALSE(handled);
    REQUIRE(handlerExecutor->pendingCount() == 1);

    handlerExecutor->runPending(1);
    REQUIRE(handled);
}

TEST_CASE("onError without an executor runs the handler on the failing task's executor", "[exception][executor]")
{
    const auto executor = ManualExecutor::create();
    std::thread::id handlerThread;

    Task::run(executor.get(), [] { throw std::runtime_error{"failed"}; })
        .onError([&handlerThread](const std::exception&) {
        handlerThread = std::this_thread::get_id();
    }).submit();

    executor->runPending(1);
    REQUIRE(handlerThread == std::this_thread::get_id());
    REQUIRE(executor->pendingCount() == 0); // run inline, not queued again
}

TEST_CASE("The onError handler of a delayed task runs on the task's executor", "[exception][timer]")
{
    // The delay goes through the timer queue; the handler must still run on the task's executor
    // (here: this thread), not on the timer thread.
    const auto executor = ManualExecutor::create();
    std::atomic<bool> handled{false};
    std::thread::id handlerThread;

    Task::run(executor.get(), 10ms, [] { throw std::runtime_error{"failed"}; })
        .onError([&handled, &handlerThread](const std::exception&) {
        handlerThread = std::this_thread::get_id();
        handled = true;
    }).submit();

    REQUIRE(waitFor([&] {
        executor->runPending(10);
        return handled.load();
    }));
    REQUIRE(handlerThread == std::this_thread::get_id());
}

TEST_CASE("An error handler can receive the chain's token", "[exception][token]")
{
    const auto executor = ManualExecutor::create();
    std::stop_token taskToken;
    std::stop_token handlerToken;

    auto handle = Task::run(executor.get(), [&taskToken](const std::stop_token& token) {
        taskToken = token;
        throw std::runtime_error{"failed"};
    }).onError([&handlerToken](const std::exception&, const std::stop_token& token) {
        handlerToken = token;
    }).submit();

    executor->runPending(1);

    REQUIRE(handlerToken.stop_possible()); // while the handle (and so the chain's stop source) exists
    REQUIRE(handlerToken == taskToken);
    handle.cancel();
    REQUIRE(handlerToken.stop_requested());
    REQUIRE_THROWS_AS(handle.get(), std::runtime_error);
}

TEST_CASE("An exception propagates through a chain of tasks with arguments to the handle", "[exception][handle]")
{
    const auto executor = ManualExecutor::create();
    bool lastRan = false;

    auto handle = Task::run(executor.get(), [] { return 1; })
                      .then(
                          executor.get(),
                          [](const int value) -> int {
        if (value == 1) {
            throw std::invalid_argument{"no ones"};
        }
        return value;
    })
                      .then(executor.get(), [&lastRan](const int value) {
        lastRan = true;
        return value * 2;
    }).submit();

    executor->runPending(10);

    REQUIRE_FALSE(lastRan);
    REQUIRE_THROWS_AS(handle.get(), std::invalid_argument);
}

/// Unhandled-exception handler
/// ================================================================================================

TEST_CASE("Non-standard exceptions are reported too", "[exception][unhandled]")
{
    const UnhandledExceptions unhandled;
    const auto executor = ManualExecutor::create();

    Task::post(executor.get(), [] { throw 42; }); // NOLINT
    executor->runPending(1);

    REQUIRE(unhandled.messages() == std::vector<std::string>{"<unknown>"});
}

TEST_CASE("The unhandled-exception handler is robust", "[exception][unhandled]")
{
    const auto previous = setUnhandledExceptionHandler(nullptr);

    SECTION("an empty handler ignores unhandled exceptions")
    {
        const auto executor = ManualExecutor::create();
        Task::post(executor.get(), [] { throw std::runtime_error{"ignored"}; });
        REQUIRE_NOTHROW(executor->runPending(1));
    }

    SECTION("a throwing handler doesn't take down the executor")
    {
        setUnhandledExceptionHandler([](const std::exception_ptr&) { throw std::logic_error{"handler failed"}; });
        const auto executor = ManualExecutor::create();
        bool secondRan = false;
        Task::post(executor.get(), [] { throw std::runtime_error{"first"}; });
        Task::post(executor.get(), [&secondRan] { secondRan = true; });

        REQUIRE_NOTHROW(executor->runPending(2));
        REQUIRE(secondRan);
    }

    SECTION("a null exception is not reported")
    {
        bool called = false;
        setUnhandledExceptionHandler([&called](const std::exception_ptr&) { called = true; });
        detail::reportUnhandledException(nullptr);
        REQUIRE_FALSE(called);
    }

    setUnhandledExceptionHandler(previous);
}

TEST_CASE("The default unhandled-exception handler writes to stderr", "[exception][unhandled]")
{
    // The default is in place unless a test replaced it (and every test restores it).
    const auto defaultHandler = setUnhandledExceptionHandler(nullptr);
    setUnhandledExceptionHandler(defaultHandler);
    REQUIRE(defaultHandler);

    const StandardErrorCapture capture;
    defaultHandler(std::make_exception_ptr(std::runtime_error{"something broke"}));
    defaultHandler(std::make_exception_ptr(42));

    REQUIRE_THAT(capture.text(), Catch::Matchers::ContainsSubstring("something broke"));
    REQUIRE_THAT(capture.text(), Catch::Matchers::ContainsSubstring("non-standard"));
}
