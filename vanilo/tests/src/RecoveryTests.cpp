#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <stdexcept>
#include <string>
#include <vector>

// recover handlers, typed onError handlers and the handle's error reporting.

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::UnhandledExceptions;
using vanilo::tests::waitFor;

namespace {
    struct NetworkError: std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };
} // namespace

/// recover
/// ================================================================================================

TEST_CASE("recover replaces the result of a failed task and the chain goes on", "[recover]")
{
    const auto executor = ManualExecutor::create();

    auto handle =
        Task::run(executor.get(), []() -> int { throw std::runtime_error{"offline"}; }).recover([](const std::exception&) {
        return 20;
    }).then(executor.get(), [](const int value) {
        return value + 1;
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 21);
}

TEST_CASE("recover is skipped when nothing fails", "[recover]")
{
    const auto executor = ManualExecutor::create();
    bool recovered = false;

    auto handle = Task::run(executor.get(), [] { return 1; })
                      .recover([&recovered](const std::exception&) {
        recovered = true;
        return 0;
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 1);
    REQUIRE_FALSE(recovered);
}

TEST_CASE("recover catches the errors of the tasks before it", "[recover]")
{
    const auto executor = ManualExecutor::create();
    bool secondRan = false;

    auto handle = Task::run(executor.get(), []() -> int { throw std::runtime_error{"first"}; })
                      .then(executor.get(), [&secondRan](const int value) {
        secondRan = true;
        return value;
    }).recover([](const std::exception& ex) {
        return std::string{ex.what()} == "first" ? 7 : 0;
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 7);
    REQUIRE_FALSE(secondRan);
}

TEST_CASE("An onError handler before a recover still observes the error", "[recover]")
{
    const auto executor = ManualExecutor::create();
    std::vector<std::string> calls;

    auto handle = Task::run(executor.get(), []() -> int { throw std::runtime_error{"failed"}; })
                      .onError([&calls](const std::exception&) { calls.emplace_back("observed"); })
                      .recover([&calls](const std::exception&) {
        calls.emplace_back("recovered");
        return 1;
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 1);
    REQUIRE(calls == std::vector<std::string>{"observed", "recovered"});
}

TEST_CASE("An error thrown by recover goes on down the chain", "[recover]")
{
    const auto executor = ManualExecutor::create();
    std::string observed;

    auto handle = Task::run(executor.get(), []() -> int { throw std::runtime_error{"first"}; })
                      .recover([](const std::exception&) -> int { throw std::logic_error{"recovery failed"}; })
                      .onError([&observed](const std::exception& ex) { observed = ex.what(); })
                      .then(executor.get(), [](const int value) {
        return value;
    }).submit();
    executor->runPending();

    REQUIRE(observed == "recovery failed");
    REQUIRE_THROWS_AS(handle.get(), std::logic_error);
}

TEST_CASE("recover on a void chain swallows the error", "[recover]")
{
    const auto executor = ManualExecutor::create();
    bool continued = false;

    auto handle = Task::run(executor.get(), [] { throw std::runtime_error{"failed"}; }).recover([](const std::exception&) {
    }).then(executor.get(), [&continued] {
        continued = true;
    }).submit();
    executor->runPending();

    REQUIRE_NOTHROW(handle.get());
    REQUIRE(continued);
}

TEST_CASE("recover can handle a cancellation", "[recover][cancel]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    source.request_stop();

    auto handle =
        Task::run(executor.get(), source.get_token(), [] { return 1; }).recover([](const OperationCanceledException&) {
        return -1;
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == -1);
}

TEST_CASE("recover runs on the executor given to it", "[recover][executor]")
{
    const auto taskExecutor = ManualExecutor::create();
    const auto recoveryExecutor = ManualExecutor::create();

    auto handle = Task::run(taskExecutor.get(), []() -> int { throw std::runtime_error{"failed"}; })
                      .recover(recoveryExecutor.get(), [](const std::exception&) { return 5; })
                      .then(taskExecutor.get(), [](const int value) {
        return value * 2;
    }).submit();

    taskExecutor->runPending();
    REQUIRE(recoveryExecutor->pendingCount() == 1);
    recoveryExecutor->runPending();
    taskExecutor->runPending();

    REQUIRE(handle.get() == 10);
}

TEST_CASE("recover in a periodic chain recovers each run", "[recover][periodic]")
{
    const auto executor = ManualExecutor::create();
    std::atomic<int> recovered{0};

    auto handle = Task::run(executor.get(), 0ms, 2ms, []() -> int { throw std::runtime_error{"run failed"}; })
                      .recover([](const std::exception&) { return 1; })
                      .then(executor.get(), [&recovered](const int value) {
        recovered += value;
    }).submit();

    REQUIRE(waitFor([&] {
        executor->runPending();
        return recovered >= 2;
    }));
    handle.cancel();
}

/// Typed handlers
/// ================================================================================================

TEST_CASE("A handler is only called for the exceptions of its parameter type", "[exception][typed]")
{
    const auto executor = ManualExecutor::create();
    std::vector<std::string> calls;

    auto handle = Task::run(executor.get(), []() -> int { throw NetworkError{"timeout"}; })
                      .onError([&calls](const std::logic_error&) { calls.emplace_back("logic"); })
                      .onError([&calls](const NetworkError& ex) { calls.emplace_back(std::string{"network: "} + ex.what()); })
                      .recover([](const std::invalid_argument&) { return 0; })
                      .recover([](const std::runtime_error&) {
        return 2;
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 2);
    REQUIRE(calls == std::vector<std::string>{"network: timeout"});
}

TEST_CASE("A handler taking a std::exception_ptr receives any error", "[exception][typed]")
{
    const auto executor = ManualExecutor::create();
    bool handled = false;

    auto handle = Task::run(executor.get(), [] { throw 42; }) // NOLINT
                      .onError([&handled](const std::exception_ptr& error) {
        try {
            std::rethrow_exception(error);
        }
        catch (const int value) {
            handled = value == 42;
        }
    }).submit();
    executor->runPending();

    REQUIRE(handled);
    REQUIRE_THROWS_AS(handle.get(), int);
}

TEST_CASE("A std::exception handler doesn't receive a non-standard exception", "[exception][typed]")
{
    const UnhandledExceptions unhandled;
    const auto executor = ManualExecutor::create();
    bool handled = false;

    Task::run(executor.get(), [] { throw 42; }) // NOLINT
        .onError([&handled](const std::exception&) {
        handled = true;
    }).submit();
    executor->runPending();

    REQUIRE_FALSE(handled);
    REQUIRE(unhandled.messages() == std::vector<std::string>{"<unknown>"});
}

TEST_CASE("A generic handler receives a std::exception", "[exception][typed]")
{
    const auto executor = ManualExecutor::create();
    std::string message;

    Task::run(executor.get(), [] { throw std::runtime_error{"generic"}; }).onError([&message](const auto& ex) {
        message = ex.what();
    }).submit();
    executor->runPending();

    REQUIRE(message == "generic");
}

/// The handle's error reporting
/// ================================================================================================

TEST_CASE("An error that nobody received is reported when the handle is dropped", "[handle][unhandled]")
{
    const UnhandledExceptions unhandled;
    const auto executor = ManualExecutor::create();

    SECTION("dropped after the chain failed")
    {
        {
            auto handle = Task::run(executor.get(), [] { throw std::runtime_error{"late drop"}; }).submit();
            executor->runPending();
            REQUIRE(unhandled.messages().empty()); // the handle could still get() it
        }
        REQUIRE(unhandled.messages() == std::vector<std::string>{"late drop"});
    }

    SECTION("dropped before the chain failed")
    {
        Task::run(executor.get(), [] { throw std::runtime_error{"early drop"}; }).submit();
        executor->runPending();
        REQUIRE(unhandled.messages() == std::vector<std::string>{"early drop"});
    }

    SECTION("waited for but not looked at")
    {
        {
            auto handle = Task::run(executor.get(), [] { throw std::runtime_error{"not looked at"}; }).submit();
            executor->runPending();
            handle.wait();
        }
        REQUIRE(unhandled.messages() == std::vector<std::string>{"not looked at"});
    }
}

TEST_CASE("An error is not reported when get() rethrew it or a handler received it", "[handle][unhandled]")
{
    const UnhandledExceptions unhandled;
    const auto executor = ManualExecutor::create();

    {
        auto handle = Task::run(executor.get(), [] { throw std::runtime_error{"seen"}; }).submit();
        executor->runPending();
        REQUIRE_THROWS_AS(handle.get(), std::runtime_error);
        REQUIRE_FALSE(handle.valid());
    }
    Task::run(executor.get(), [] { throw std::runtime_error{"handled"}; }).onError([](const std::exception&) {}).submit();
    executor->runPending();

    REQUIRE(unhandled.messages().empty());
}

TEST_CASE("TaskHandle waits with a timeout", "[handle]")
{
    const auto executor = ManualExecutor::create();
    auto handle = Task::run(executor.get(), [] { return 1; }).submit();

    REQUIRE_FALSE(handle.waitFor(10ms));
    REQUIRE_FALSE(handle.waitUntil(std::chrono::system_clock::now() + 10ms));

    std::thread runner{[&executor] {
        std::this_thread::sleep_for(20ms);
        executor->runPending();
    }};
    REQUIRE(handle.waitUntil(std::chrono::steady_clock::now() + 5s));
    runner.join();

    REQUIRE(handle.isReady());
    REQUIRE(handle.get() == 1);
}

TEST_CASE("A builder can only be used once", "[builder]")
{
    const auto executor = ManualExecutor::create();
    auto builder = Task::run(executor.get(), [] { return 1; });
    auto handle = std::move(builder).submit();

    REQUIRE_THROWS_AS(std::move(builder).submit(), std::logic_error); // NOLINT(bugprone-use-after-move)
    executor->runPending();
    REQUIRE(handle.get() == 1);
}
