#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <thread>

using namespace vanilo::tasker;
using namespace std::chrono_literals;

TEST_CASE("Race condition: Executor destroyed before delayed task submission", "[LifeCycleGuard]")
{
    // Create an executor that will suffer
    auto executor = ThreadPoolExecutor::create(1);
    std::atomic_bool executed{false};

    // Schedule a task on the executor with a delay
    // The delay is handled by the Scheduler. When a delay passes, it chains to the executor.
    Task::run(executor.get(), vanilo::concurrent::CancellationToken::none(), 100ms, [&executed] { executed = true; });

    // Destroy the executor immediately
    executor.reset();

    // Wait for the delay to pass + some buffer
    std::this_thread::sleep_for(200ms);

    // The task should NOT have executed (because the executor is dead), and we should NOT crash.
    REQUIRE_FALSE(executed);
}
