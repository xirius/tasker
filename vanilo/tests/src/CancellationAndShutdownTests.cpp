#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

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

TEST_CASE("Stopping the token between two chained tasks cancels the rest of the chain", "[cancel][chain]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    bool secondRan = false;

    SECTION("the handle receives the cancellation")
    {
        auto handle = Task::run(
                          executor.get(), source.get_token(),
                          [&source] {
            source.request_stop();
            return 1;
        })
                          .then(executor.get(), [&secondRan](const int value) {
            secondRan = true;
            return value;
        }).submit();

        executor->runPending(10);

        REQUIRE_FALSE(secondRan);
        REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    }

    SECTION("an onError handler receives the cancellation")
    {
        bool canceled = false;
        Task::run(executor.get(), source.get_token(), [&source] { source.request_stop(); })
            .then(executor.get(), [&secondRan] { secondRan = true; })
            .onError([&canceled](const std::exception& ex) {
            canceled = isCancellation(ex);
        }).submit();

        executor->runPending(10);

        REQUIRE_FALSE(secondRan);
        REQUIRE(canceled);
    }

    SECTION("through the handle")
    {
        TaskHandle<int> handle;
        handle = Task::run(
                     executor.get(),
                     [&handle] {
            handle.cancel();
            return 1;
        })
                     .then(executor.get(), [&secondRan](const int value) {
            secondRan = true;
            return value;
        }).submit();

        executor->runPending(10);

        REQUIRE_FALSE(secondRan);
        REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    }
}

TEST_CASE("A delayed task canceled before it is due delivers the cancellation", "[cancel][timer]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    bool ran = false;

    auto handle = Task::run(executor.get(), source.get_token(), 20ms, [&ran] {
        ran = true;
        return 1;
    }).submit();
    source.request_stop();

    REQUIRE(handle.waitFor(5s));
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    executor->runPending(10);
    REQUIRE_FALSE(ran);
}

TEST_CASE("Canceling a periodic task stops it; only a run that was already queued reports the cancellation", "[cancel][periodic]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    std::atomic<int> runs{0};
    std::atomic<int> cancellations{0};

    auto handle = Task::run(executor.get(), source.get_token(), 0ms, 5ms, [&runs] { ++runs; })
                      .onError([&cancellations](const std::exception& ex) {
        if (isCancellation(ex)) {
            ++cancellations;
        }
    }).submit();

    REQUIRE(waitFor([&] {
        executor->runPending(10);
        return runs >= 2;
    }));

    source.request_stop();
    const int runsAtCancel = runs;
    REQUIRE(handle.waitFor(5s)); // the series has ended
    std::this_thread::sleep_for(30ms);
    executor->runPending(100);

    REQUIRE(runs == runsAtCancel);          // a queued run is canceled, not executed
    REQUIRE(cancellations <= 1);            // and it is the only one that reports the cancellation
    REQUIRE(executor->pendingCount() == 0); // nothing is scheduled any more
}

/// Executor shutdown
/// ================================================================================================

TEST_CASE("Destroying an executor cancels chained tasks waiting for their turn", "[shutdown][chain]")
{
    auto executor = ManualExecutor::create();

    SECTION("a task taking an argument, with an onError handler")
    {
        bool canceled = false;
        Task::run(executor.get(), [] { return 1; }).then(executor.get(), [](int) {
        }).onError([&canceled](const std::exception& ex) {
            canceled = isCancellation(ex);
        }).submit();

        executor->runPending(1); // the first task runs and queues the second
        REQUIRE(executor->pendingCount() == 1);
        executor.reset();

        REQUIRE(canceled);
    }

    SECTION("a task taking an argument and returning a value, with a handle")
    {
        auto handle = Task::run(executor.get(), [] { return 1; }).then(executor.get(), [](const int value) { return value; }).submit();

        executor->runPending(1);
        executor.reset();

        REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    }

    SECTION("a task taking an argument and returning nothing, with a handle")
    {
        auto handle = Task::run(executor.get(), [] { return 1; }).then(executor.get(), [](int) {}).submit();

        executor->runPending(1);
        executor.reset();

        REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    }

    SECTION("a task without arguments returning nothing, with a handle")
    {
        auto handle = Task::run(executor.get(), [] {}).then(executor.get(), [] {}).submit();

        executor->runPending(1);
        executor.reset();

        REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
    }
}

TEST_CASE("Destroying a ThreadPoolExecutor cancels tasks that are still queued", "[shutdown][pool]")
{
    auto executor = ThreadPoolExecutor::create(1);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> blocking{false};

    // Keep the only thread busy, so the next task stays queued until the executor is destroyed.
    Task::post(executor.get(), [released, &blocking] {
        blocking = true;
        released.wait();
    });
    REQUIRE(waitFor([&blocking] { return blocking.load(); }));

    auto handle = Task::run(executor.get(), [] { return 1; }).submit();

    std::thread destroyer{[&executor] { executor.reset(); }};
    std::this_thread::sleep_for(20ms);
    release.set_value();
    destroyer.join();

    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("A task submitted directly to an executor being destroyed is canceled", "[shutdown][submit]")
{
    // During destruction a pending task's handler submits a Task object straight to the executor; its
    // queue is already closed, so the task must be canceled rather than lost.
    std::atomic<int> runs{0};
    std::atomic<bool> canceled{false};

    SECTION("ManualExecutor")
    {
        auto executor = ManualExecutor::create();
        auto* raw = executor.get();
        Task::run(executor.get(), [] {})
            .onError([raw, &runs, &canceled](const std::exception&) {
            raw->submit(std::make_unique<RecordingTask>(runs, canceled));
        }).submit();
        executor.reset();
    }

    SECTION("ThreadPoolExecutor")
    {
        auto executor = ThreadPoolExecutor::create(1);
        auto* raw = executor.get();
        std::promise<void> release;
        auto released = release.get_future().share();
        std::atomic<bool> blocking{false};
        Task::post(executor.get(), [released, &blocking] {
            blocking = true;
            released.wait();
        });
        REQUIRE(waitFor([&blocking] { return blocking.load(); }));
        Task::run(executor.get(), [] {})
            .onError([raw, &runs, &canceled](const std::exception&) {
            raw->submit(std::make_unique<RecordingTask>(runs, canceled));
        }).submit();

        std::thread destroyer{[&executor] { executor.reset(); }};
        std::this_thread::sleep_for(20ms);
        release.set_value();
        destroyer.join();
    }

    REQUIRE(canceled);
    REQUIRE(runs == 0); // canceled instead of run
}

TEST_CASE("Task::run without an executor cancels the task", "[shutdown][submit]")
{
    auto handle = Task::run(static_cast<TaskExecutor*>(nullptr), [] { return 1; }).submit();
    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("Task::post without an executor drops the function", "[shutdown][post]")
{
    bool ran = false;
    Task::post(static_cast<TaskExecutor*>(nullptr), [&ran] { ran = true; });
    REQUIRE_FALSE(ran);
}
