#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>
#include <vanilo/tasker/TimerQueue.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;

// Regression tests for the bugs fixed in v1.0-b4, ported to v1.0. Each test describes the behaviour it protects.

using vanilo::tests::runsWithin;
using vanilo::tests::UnhandledExceptions;
using vanilo::tests::waitFor;

/// Exceptions in task chains
/// ================================================================================================

TEST_CASE("An exception skips the rest of the chain and reaches a later onError handler", "[regression][exception]")
{
    auto executor = ManualExecutor::create();
    bool secondRan = false;
    std::string handled;

    Task::run(executor.get(), [] { throw std::runtime_error{"first failed"}; }).then(executor.get(), [&secondRan] {
        secondRan = true;
    }).onError([&handled](const std::exception& ex) {
        handled = ex.what();
    }).submit();

    executor->runPending(10);

    REQUIRE_FALSE(secondRan);
    REQUIRE(handled == "first failed");
}

TEST_CASE("The nearest onError handler handles an exception, later handlers are not called", "[regression][exception]")
{
    auto executor = ManualExecutor::create();
    int handledBy = 0;
    bool thirdRan = false;

    Task::run(executor.get(), [] { throw std::runtime_error{"failed"}; }).onError([&handledBy](const std::exception&) {
        handledBy = 1;
    }).then(executor.get(), [&thirdRan] {
        thirdRan = true;
    }).onError([&handledBy](const std::exception&) {
        handledBy = 2;
    }).submit();

    executor->runPending(10);

    REQUIRE(handledBy == 1);
    REQUIRE_FALSE(thirdRan);
}

TEST_CASE("The handle of a chain receives the exception even when a handler ran", "[regression][exception][handle]")
{
    auto executor = ManualExecutor::create();
    bool handled = false;

    auto handle = Task::run(executor.get(), []() -> int { throw std::runtime_error{"failed"}; })
                      .onError([&handled](const std::exception&) { handled = true; })
                      .then(executor.get(), [](const int value) {
        return value * 2;
    }).submit();

    executor->runPending(10);

    REQUIRE(handled);
    REQUIRE_THROWS_AS(handle.get(), std::runtime_error);
}

TEST_CASE("Unhandled task exceptions are reported to the unhandled-exception handler", "[regression][exception]")
{
    const UnhandledExceptions unhandled;

    SECTION("from a posted function on a ManualExecutor")
    {
        auto executor = ManualExecutor::create();
        Task::post(executor.get(), [] { throw std::runtime_error{"local"}; });
        executor->runPending(1);

        REQUIRE(unhandled.messages() == std::vector<std::string>{"local"});
    }

    SECTION("from a posted function on a ThreadPoolExecutor")
    {
        const auto executor = ThreadPoolExecutor::create(1);
        Task::post(executor.get(), [] { throw std::runtime_error{"pool"}; });

        REQUIRE(waitFor([&unhandled] { return !unhandled.messages().empty(); }));
        REQUIRE(unhandled.messages() == std::vector<std::string>{"pool"});
    }

    SECTION("from a chain whose handle was dropped")
    {
        auto executor = ManualExecutor::create();
        Task::run(executor.get(), [] { throw std::runtime_error{"dropped"}; }).submit();
        executor->runPending(1);

        REQUIRE(unhandled.messages() == std::vector<std::string>{"dropped"});
    }

    SECTION("when an onError handler throws")
    {
        auto executor = ManualExecutor::create();
        Task::run(executor.get(), [] { throw std::runtime_error{"task"}; })
            .onError([](const std::exception&) {
            throw std::logic_error{"handler"};
        }).submit();
        executor->runPending(10);

        REQUIRE(unhandled.messages() == std::vector<std::string>{"handler"});
    }

    SECTION("but not cancellations")
    {
        auto executor = ManualExecutor::create();
        std::stop_source source;
        source.request_stop();
        Task::run(executor.get(), source.get_token(), [] {}).submit();
        Task::post(executor.get(), source.get_token(), [] {});
        executor->runPending(10);

        REQUIRE(unhandled.messages().empty());
    }
}

/// Executor shutdown
/// ================================================================================================

TEST_CASE("Destroying a ManualExecutor cancels its pending tasks", "[regression][executor][manual]")
{
    auto executor = ManualExecutor::create();
    bool ran = false;
    bool canceled = false;

    auto handle = Task::run(executor.get(), [] { return 1; }).submit();
    Task::run(executor.get(), [&ran] { ran = true; }).onError([&canceled](const OperationCanceledException&) { canceled = true; }).submit();

    executor.reset();

    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    REQUIRE_FALSE(ran);
    REQUIRE(canceled);
}

TEST_CASE("A task handed to an executor that is gone is canceled", "[regression][executor]")
{
    // The chain holds the second executor's core, so handing it the task after the executor was destroyed
    // cancels the task instead of using a dangling executor.
    const auto first = ManualExecutor::create();
    auto second = ManualExecutor::create();
    bool ran = false;

    auto handle = Task::run(first.get(), [] { return 1; })
                      .then(second.get(), [&ran](const int value) {
        ran = true;
        return value;
    }).submit();

    second.reset();
    first->runPending();

    REQUIRE_FALSE(ran);
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

namespace {
    class RecordingEntry final: public vanilo::tasker::detail::TimerEntry
    {
      public:
        RecordingEntry(const std::chrono::steady_clock::time_point due, std::atomic<int>& fired, std::atomic<int>& canceled)
            : TimerEntry{due}, _fired{fired}, _canceled{canceled}
        {
        }

        [[nodiscard]] bool isCanceled() const noexcept override
        {
            return false;
        }

        void fire(std::unique_ptr<TimerEntry>) noexcept override
        {
            ++_fired;
        }

        void cancel(std::unique_ptr<TimerEntry>) noexcept override
        {
            ++_canceled;
        }

      private:
        std::atomic<int>& _fired;
        std::atomic<int>& _canceled;
    };
} // namespace

TEST_CASE("Shutting down the timer queue cancels its pending entries", "[regression][timer]")
{
    auto queue = std::make_unique<vanilo::tasker::detail::TimerQueue>();
    std::atomic<int> fired{0};
    std::atomic<int> canceled{0};

    queue->schedule(std::make_unique<RecordingEntry>(std::chrono::steady_clock::now() + 1h, fired, canceled));
    queue->schedule(std::make_unique<RecordingEntry>(std::chrono::steady_clock::now() + 2h, fired, canceled));
    REQUIRE(queue->pendingCount() == 2);

    queue.reset();

    REQUIRE(fired == 0);
    REQUIRE(canceled == 2);
}

TEST_CASE("A timer queue refuses entries once it has shut down", "[regression][timer]")
{
    vanilo::tasker::detail::TimerQueue queue;
    std::atomic<int> fired{0};
    std::atomic<int> canceled{0};

    queue.shutdown();
    auto refused = queue.schedule(std::make_unique<RecordingEntry>(std::chrono::steady_clock::now(), fired, canceled));

    REQUIRE(refused != nullptr);
    REQUIRE(queue.pendingCount() == 0);
}

TEST_CASE("A timer queue can be created and destroyed repeatedly without hanging", "[regression][timer][stress]")
{
    // The b4 scheduler's destructor set the stop flag without holding the mutex, so the worker could miss
    // the wake-up and wait forever, hanging the destructor's join().
    REQUIRE(runsWithin([] {
        for (int i = 0; i < 2000; ++i) {
            vanilo::tasker::detail::TimerQueue queue;
        }
    }, 60s));
}

/// Periodic tasks
/// ================================================================================================

TEST_CASE("A periodic task needs a positive interval", "[regression][periodic]")
{
    const auto executor = ManualExecutor::create();

    REQUIRE_THROWS_AS(Task::run(executor.get(), std::stop_token{}, 0ms, 0ms, [] {}).submit(), std::invalid_argument);
    REQUIRE_THROWS_AS(Task::run(executor.get(), 0ms, -1ms, [] {}).submit(), std::invalid_argument);
    REQUIRE_THROWS_AS(Task::run(executor.get(), 0ms, FixedDelay{0ms}, [] {}).submit(), std::invalid_argument);
    REQUIRE(executor->pendingCount() == 0);
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
    // b4 applied FunctionTraits to the deduced reference type, so only temporaries compiled.
    const auto executor = ManualExecutor::create();

    auto lambda = [] { return 1; };
    const auto constLambda = [](const int value) { return value + 1; };
    auto* functionPointer = &answer;
    std::function<int(int)> function = [](const int value) { return value * 2; };
    bool handled = false;
    auto handler = [&handled](const std::exception&) { handled = true; };

    auto chained = Task::run(executor.get(), lambda).then(executor.get(), constLambda).then(executor.get(), function).submit();
    auto fromPointer = Task::run(executor.get(), functionPointer).submit();
    auto fromFunction = Task::run(executor.get(), answer).submit();
    Task::run(executor.get(), [] { throw std::runtime_error{"failed"}; }).onError(handler).submit();

    executor->runPending(10);

    REQUIRE(chained.get() == 4);
    REQUIRE(fromPointer.get() == 42);
    REQUIRE(fromFunction.get() == 42);
    REQUIRE(handled);
}
