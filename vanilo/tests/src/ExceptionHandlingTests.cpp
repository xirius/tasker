#include <vanilo/tasker/Tasker.h>

#include "TestUtilities.h"

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

/// Where onException handlers run
/// ================================================================================================

TEST_CASE("onException with an executor runs the handler on that executor", "[exception][executor]")
{
    const auto taskExecutor = LocalThreadExecutor::create();
    const auto handlerExecutor = LocalThreadExecutor::create();
    bool handled = false;

    Task::run(taskExecutor.get(), [] { throw std::runtime_error{"failed"}; })
        .onException(handlerExecutor.get(), [&handled](const std::exception&) { handled = true; });

    taskExecutor->process(1);
    REQUIRE_FALSE(handled);
    REQUIRE(handlerExecutor->count() == 1);

    handlerExecutor->process(1);
    REQUIRE(handled);
}

TEST_CASE("onException without an executor runs the handler on the failing task's executor", "[exception][executor]")
{
    const auto executor = LocalThreadExecutor::create();
    std::thread::id handlerThread;

    Task::run(executor.get(), [] { throw std::runtime_error{"failed"}; }).onException([&handlerThread](const std::exception&) {
        handlerThread = std::this_thread::get_id();
    });

    executor->process(1);
    REQUIRE(handlerThread == std::this_thread::get_id());
    REQUIRE(executor->count() == 0); // run inline, not queued again
}

TEST_CASE("The onException handler of a delayed task runs on the task's executor", "[exception][scheduler]")
{
    // The delay goes through the internal scheduler; the handler must still run on the task's
    // executor (here: this thread), not on the scheduler's thread.
    const auto executor = LocalThreadExecutor::create();
    std::atomic<bool> handled{false};
    std::thread::id handlerThread;

    Task::run(executor.get(), CancellationToken{}, 10ms, [] { throw std::runtime_error{"failed"}; })
        .onException([&handled, &handlerThread](const std::exception&) {
            handlerThread = std::this_thread::get_id();
            handled = true;
        });

    REQUIRE(waitFor([&] {
        executor->process(10);
        return handled.load();
    }));
    REQUIRE(handlerThread == std::this_thread::get_id());
}

TEST_CASE("An exception handler can receive the cancellation token and the task's arguments", "[exception][binder]")
{
    const auto executor = LocalThreadExecutor::create();
    CancellationTokenSource source;
    int counter = 0;
    bool sawToken = false;

    Task::run(
        executor.get(), source.token(), [](const CancellationToken&, int&) { throw std::runtime_error{"failed"}; }, std::ref(counter))
        .onException([&sawToken, &source](const std::exception&, const CancellationToken& token, int& boundCounter) {
            sawToken = token == source.token();
            boundCounter = -1;
        });

    executor->process(1);

    REQUIRE(sawToken);
    REQUIRE(counter == -1);
}

TEST_CASE("An exception propagates through a chain of tasks with arguments to the future", "[exception][future]")
{
    const auto executor = LocalThreadExecutor::create();
    bool lastRan = false;

    auto future = Task::run(executor.get(), [] { return 1; })
                      .then(executor.get(), [](const int value) -> int {
                          if (value == 1) {
                              throw std::invalid_argument{"no ones"};
                          }
                          return value;
                      })
                      .then(executor.get(), [&lastRan](const int value) {
                          lastRan = true;
                          return value * 2;
                      })
                      .getFuture();

    executor->process(10);

    REQUIRE_FALSE(lastRan);
    REQUIRE_THROWS_AS(future.get(), std::invalid_argument);
}

/// Unhandled-exception handler
/// ================================================================================================

TEST_CASE("Non-standard exceptions are reported too", "[exception][unhandled]")
{
    const UnhandledExceptions unhandled;
    const auto executor = LocalThreadExecutor::create();

    Task::run(executor.get(), [] { throw 42; }); // NOLINT
    executor->process(1);

    REQUIRE(unhandled.messages() == std::vector<std::string>{"<unknown>"});
}

TEST_CASE("The unhandled-exception handler is robust", "[exception][unhandled]")
{
    const auto previous = setUnhandledExceptionHandler(nullptr);

    SECTION("an empty handler ignores unhandled exceptions")
    {
        const auto executor = LocalThreadExecutor::create();
        Task::run(executor.get(), [] { throw std::runtime_error{"ignored"}; });
        REQUIRE_NOTHROW(executor->process(1));
    }

    SECTION("a throwing handler doesn't take down the executor")
    {
        setUnhandledExceptionHandler([](const std::exception_ptr&) { throw std::logic_error{"handler failed"}; });
        const auto executor = LocalThreadExecutor::create();
        bool secondRan = false;
        Task::run(executor.get(), [] { throw std::runtime_error{"first"}; });
        Task::run(executor.get(), [&secondRan] { secondRan = true; });

        REQUIRE_NOTHROW(executor->process(2));
        REQUIRE(secondRan);
    }

    SECTION("a null exception is not reported")
    {
        bool called = false;
        setUnhandledExceptionHandler([&called](const std::exception_ptr&) { called = true; });
        internal::reportUnhandledException(nullptr);
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
