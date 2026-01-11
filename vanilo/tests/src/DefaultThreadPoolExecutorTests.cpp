#include <vanilo/tasker/DefaultThreadPoolExecutor.h>
#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace vanilo::tasker;
using namespace std::chrono_literals;

TEST_CASE("Destructor race with resize", "[DefaultThreadPoolExecutor]")
{
    for (int i = 0; i < 100; ++i) {
        auto executor = std::make_unique<DefaultThreadPoolExecutor>(4);
        std::atomic<bool> running{true};

        std::thread thread([&running, &executor] {
            while (running) {
                executor->resize(8);
                executor->resize(2);
            }
        });

        std::this_thread::sleep_for(1ms);
        running = false;

        if (thread.joinable()) {
            thread.join();
        }

        executor.reset();
    }
}

TEST_CASE("Bug: StopThreadTask efficiency", "[DefaultThreadPoolExecutor]")
{
    DefaultThreadPoolExecutor executor(10);
    // If we resize down to 1, 9 StopThreadTasks are submitted.
    // They will keep re-submitting themselves until they hit the right thread.
    auto future = executor.resize(1);
    future.get();
    REQUIRE(executor.threadCount() == 1);
}

class HeavyTask: public Task
{
  public:
    void run() override
    {
        std::this_thread::sleep_for(10ms);
    }
    void cancel() noexcept override
    {
        // Ignore
    }
};

TEST_CASE("Bug: StopThreadTask starvation", "[DefaultThreadPoolExecutor]")
{
    DefaultThreadPoolExecutor executor(2);
    // Fill the queue with heavy tasks
    for (int i = 0; i < 100; ++i) {
        executor.submit(std::make_unique<HeavyTask>());
    }
    // Now resize down. The StopThreadTask will be at the end of the queue.
    // When it runs, if it's on the wrong thread, it re-submits to the end again.
    const auto start = std::chrono::steady_clock::now();
    auto future = executor.resize(1);
    future.get();
    const auto end = std::chrono::steady_clock::now();

    // It should not take too long, but with current implementation it might take a while
    // as it has to wait for all HeavyTasks before each attempt.
    CHECK((end - start) < 2s);
}
