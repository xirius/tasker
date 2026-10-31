#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::waitFor;

TEST_CASE("Destructor race with resize", "[ThreadPoolExecutor]")
{
    for (int i = 0; i < 100; ++i) {
        auto executor = ThreadPoolExecutor::create(4);
        std::atomic<bool> running{true};

        std::thread thread([&running, raw = executor.get()] {
            while (running) {
                raw->resize(8);
                raw->resize(2);
            }
        });

        std::this_thread::sleep_for(1ms);
        running = false;
        thread.join();

        executor.reset();
    }
}

TEST_CASE("Shrinking the pool stops the threads promptly", "[ThreadPoolExecutor]")
{
    const auto executor = ThreadPoolExecutor::create(10);
    auto future = executor->resize(1);
    future.get();
    REQUIRE(executor->threadCount() == 1);
}

TEST_CASE("Shrinking a busy pool doesn't wait for its queued tasks", "[ThreadPoolExecutor]")
{
    const auto executor = ThreadPoolExecutor::create(2);
    std::atomic<int> done{0};

    // Fill the queues with slow tasks
    for (int i = 0; i < 100; ++i) {
        Task::post(executor.get(), [&done] {
            std::this_thread::sleep_for(10ms);
            ++done;
        });
    }

    // A removed thread finishes its current task only; the queued ones stay for the remaining thread.
    const auto start = std::chrono::steady_clock::now();
    executor->resize(1).get();
    CHECK((std::chrono::steady_clock::now() - start) < 2s);
    REQUIRE(executor->threadCount() == 1);
    REQUIRE(done < 100);
}

TEST_CASE("The tasks queued for a removed thread still run", "[ThreadPoolExecutor]")
{
    const auto executor = ThreadPoolExecutor::create(4);
    std::atomic<int> done{0};

    for (int i = 0; i < 200; ++i) {
        Task::post(executor.get(), [&done] {
            std::this_thread::sleep_for(1ms);
            ++done;
        });
    }
    executor->resize(1).get();

    REQUIRE(waitFor([&done] { return done == 200; }, 10s));
}

TEST_CASE("A pool can shrink to no threads and grow again", "[ThreadPoolExecutor]")
{
    const auto executor = ThreadPoolExecutor::create(2);
    std::atomic<int> done{0};

    executor->resize(0).get();
    Task::post(executor.get(), [&done] { ++done; });
    std::this_thread::sleep_for(20ms);
    REQUIRE(done == 0); // no thread to run it
    REQUIRE(executor->pendingCount() == 1);

    executor->resize(1);
    REQUIRE(waitFor([&done] { return done == 1; }));
}

TEST_CASE("A pool can be destroyed by one of its own tasks", "[ThreadPoolExecutor]")
{
    auto executor = ThreadPoolExecutor::create(2);
    std::atomic<bool> destroyed{false};

    Task::post(executor.get(), [&executor, &destroyed] {
        executor.reset();
        destroyed = true;
    });

    REQUIRE(waitFor([&destroyed] { return destroyed.load(); }));
}

TEST_CASE("Tasks queued by a pool thread are shared with the idle threads", "[ThreadPoolExecutor][stealing]")
{
    const auto executor = ThreadPoolExecutor::create(4);
    std::atomic<int> done{0};
    const auto start = std::chrono::steady_clock::now();

    // One task queues the others on its own thread's queue: the other threads must take them.
    Task::post(executor.get(), [&executor, &done] {
        for (int i = 0; i < 8; ++i) {
            Task::post(executor.get(), [&done] {
                std::this_thread::sleep_for(50ms);
                ++done;
            });
        }
    });

    REQUIRE(waitFor([&done] { return done == 8; }));
    CHECK((std::chrono::steady_clock::now() - start) < 300ms); // 8 × 50 ms one after the other would be 400 ms
}

TEST_CASE("A pool task can wait for a task it queued", "[ThreadPoolExecutor][stealing]")
{
    const auto executor = ThreadPoolExecutor::create(2);
    std::this_thread::sleep_for(20ms); // let the other thread fall asleep

    auto outer = Task::run(executor.get(), [&executor] {
        auto inner = Task::run(executor.get(), [] { return 21; }).submit();
        return inner.get() * 2; // the other thread takes the inner task from this thread's queue
    }).submit();

    REQUIRE(outer.waitFor(5s));
    REQUIRE(outer.get() == 42);
}
