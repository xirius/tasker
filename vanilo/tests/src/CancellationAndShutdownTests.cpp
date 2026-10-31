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
using vanilo::tests::waitFor;

namespace {
    bool isCancellation(const std::exception& ex)
    {
        return dynamic_cast<const OperationCanceledException*>(&ex) != nullptr;
    }
} // namespace

/// Cancellation through the token
/// ================================================================================================

TEST_CASE("Canceling the token between two chained tasks cancels the rest of the chain", "[cancel][chain]")
{
    const auto executor = LocalThreadExecutor::create();
    CancellationTokenSource source;
    bool secondRan = false;

    SECTION("a future at the end receives the cancellation")
    {
        auto future = Task::run(executor.get(), source.token(),
                                [&source] {
                                    source.cancel();
                                    return 1;
                                })
                          .then(executor.get(), [&secondRan](const int value) {
                              secondRan = true;
                              return value;
                          })
                          .getFuture();

        executor->process(10);

        REQUIRE_FALSE(secondRan);
        REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
    }

    SECTION("an onException handler receives the cancellation")
    {
        bool canceled = false;
        Task::run(executor.get(), source.token(), [&source] { source.cancel(); })
            .then(executor.get(), [&secondRan] { secondRan = true; })
            .onException([&canceled](const std::exception& ex) { canceled = isCancellation(ex); });

        executor->process(10);

        REQUIRE_FALSE(secondRan);
        REQUIRE(canceled);
    }
}

TEST_CASE("A delayed task canceled before it is due delivers the cancellation", "[cancel][scheduler]")
{
    const auto executor = LocalThreadExecutor::create();
    CancellationTokenSource source;
    bool ran = false;

    auto future = Task::run(executor.get(), source.token(), 20ms, [&ran] {
                      ran = true;
                      return 1;
                  }).getFuture();
    source.cancel();

    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
    executor->process(10);
    REQUIRE_FALSE(ran);
}

TEST_CASE("Canceling a periodic task stops it; only a run that was already queued reports the cancellation", "[cancel][periodic]")
{
    const auto executor = LocalThreadExecutor::create();
    CancellationTokenSource source;
    std::atomic<int> runs{0};
    std::atomic<int> cancellations{0};

    Task::run(executor.get(), source.token(), 0ms, 5ms, [&runs] { ++runs; }).onException([&cancellations](const std::exception& ex) {
        if (isCancellation(ex)) {
            ++cancellations;
        }
    });

    REQUIRE(waitFor([&] {
        executor->process(10);
        return runs >= 2;
    }));

    source.cancel();
    const int runsAtCancel = runs;
    std::this_thread::sleep_for(30ms);
    executor->process(100);
    std::this_thread::sleep_for(30ms);
    executor->process(100);

    REQUIRE(runs == runsAtCancel);  // a queued run is canceled, not executed
    REQUIRE(cancellations <= 1);    // and it is the only one that reports the cancellation
    REQUIRE(executor->count() == 0); // nothing is scheduled any more
}

/// Executor shutdown
/// ================================================================================================

TEST_CASE("Destroying an executor cancels chained tasks waiting for their turn", "[shutdown][chain]")
{
    auto executor = LocalThreadExecutor::create();

    SECTION("a task taking an argument, with an onException handler")
    {
        bool canceled = false;
        Task::run(executor.get(), [] { return 1; })
            .then(executor.get(), [](int) {})
            .onException([&canceled](const std::exception& ex) { canceled = isCancellation(ex); });

        executor->process(1); // the first task runs and queues the second
        REQUIRE(executor->count() == 1);
        executor.reset();

        REQUIRE(canceled);
    }

    SECTION("a task taking an argument and returning a value, with a future")
    {
        auto future = Task::run(executor.get(), [] { return 1; }).then(executor.get(), [](const int value) { return value; }).getFuture();

        executor->process(1);
        executor.reset();

        REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
    }

    SECTION("a task taking an argument and returning nothing, with a future")
    {
        auto future = Task::run(executor.get(), [] { return 1; }).then(executor.get(), [](int) {}).getFuture();

        executor->process(1);
        executor.reset();

        REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
    }

    SECTION("a task without arguments returning nothing, with a future")
    {
        auto future = Task::run(executor.get(), [] {}).then(executor.get(), [] {}).getFuture();

        executor->process(1);
        executor.reset();

        REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
    }
}

TEST_CASE("Destroying a ThreadPoolExecutor cancels tasks that are still queued", "[shutdown][pool]")
{
    auto executor = ThreadPoolExecutor::create(1);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> blocking{false};

    // Keep the only thread busy, so the next task stays queued until the executor is destroyed.
    Task::run(executor.get(), [released, &blocking] {
        blocking = true;
        released.wait();
    });
    REQUIRE(waitFor([&blocking] { return blocking.load(); }));

    auto future = Task::run(executor.get(), [] { return 1; }).getFuture();

    std::thread destroyer{[&executor] { executor.reset(); }};
    std::this_thread::sleep_for(20ms);
    release.set_value();
    destroyer.join();

    REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
}

TEST_CASE("A task submitted directly to an executor being destroyed is canceled", "[shutdown][submit]")
{
    // During destruction a pending task's handler submits a plain Task straight to the executor;
    // its queue is already closed, so the task must be canceled rather than lost.
    std::atomic<int> runs{0};
    std::atomic<bool> canceled{false};

    SECTION("LocalThreadExecutor")
    {
        auto executor = LocalThreadExecutor::create();
        auto* raw = executor.get();
        Task::run(executor.get(), [] {}).onException([raw, &runs, &canceled](const std::exception&) {
            raw->submit(std::make_unique<RecordingTask>(runs, canceled));
        });
        executor.reset();
    }

    SECTION("ThreadPoolExecutor")
    {
        auto executor = ThreadPoolExecutor::create(1);
        auto* raw = executor.get();
        std::promise<void> release;
        auto released = release.get_future().share();
        std::atomic<bool> blocking{false};
        Task::run(executor.get(), [released, &blocking] {
            blocking = true;
            released.wait();
        });
        REQUIRE(waitFor([&blocking] { return blocking.load(); }));
        Task::run(executor.get(), [] {}).onException([raw, &runs, &canceled](const std::exception&) {
            raw->submit(std::make_unique<RecordingTask>(runs, canceled));
        });

        std::thread destroyer{[&executor] { executor.reset(); }};
        std::this_thread::sleep_for(20ms);
        release.set_value();
        destroyer.join();
    }

    REQUIRE(canceled);
    REQUIRE(runs == 1); // a canceled task still runs once, in the canceled state
}

TEST_CASE("Task::run without an executor cancels the task", "[shutdown][submit]")
{
    auto future = Task::run(static_cast<TaskExecutor*>(nullptr), [] { return 1; }).getFuture();
    REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
}
