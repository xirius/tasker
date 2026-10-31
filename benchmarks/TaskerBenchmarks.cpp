// Benchmarks of the task runner. Each benchmark describes what it measures; build the benchmark preset and
// start bin/Vanilo.benchmarks.

#include <vanilo/tasker/Tasker.h>

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <thread>
#include <vector>

using namespace vanilo::tasker;

namespace {
    constexpr int TasksPerIteration = 10'000;

    void waitUntil(const std::atomic<int>& counter, const int expected)
    {
        while (counter.load(std::memory_order_acquire) < expected) {
            std::this_thread::yield();
        }
    }
} // namespace

/// Thread pool
/// ================================================================================================

// One producer queues many small tasks; measures queueing plus execution throughput.
void ThreadPool_SingleProducer(benchmark::State& state)
{
    const auto executor = ThreadPoolExecutor::create(static_cast<size_t>(state.range(0)));

    for (auto _ : state) {
        std::atomic<int> done{0};
        for (int i = 0; i < TasksPerIteration; ++i) {
            Task::post(executor.get(), [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        }
        waitUntil(done, TasksPerIteration);
    }
    state.SetItemsProcessed(state.iterations() * TasksPerIteration);
}
BENCHMARK(ThreadPool_SingleProducer)->Arg(1)->Arg(4)->Arg(16)->UseRealTime();

// Several threads queue tasks at the same time; measures contention on the pool's queues.
void ThreadPool_FourProducers(benchmark::State& state)
{
    const auto executor = ThreadPoolExecutor::create(static_cast<size_t>(state.range(0)));
    constexpr int Producers = 4;

    for (auto _ : state) {
        std::atomic<int> done{0};
        std::vector<std::thread> producers;
        for (int producer = 0; producer < Producers; ++producer) {
            producers.emplace_back([&executor, &done] {
                for (int i = 0; i < TasksPerIteration / Producers; ++i) {
                    Task::post(executor.get(), [&done] { done.fetch_add(1, std::memory_order_relaxed); });
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }
        waitUntil(done, TasksPerIteration);
    }
    state.SetItemsProcessed(state.iterations() * TasksPerIteration);
}
BENCHMARK(ThreadPool_FourProducers)->Arg(4)->Arg(16)->UseRealTime();

// Tasks that queue more tasks from inside the pool (fan-out), as continuations do.
void ThreadPool_TasksQueueTasks(benchmark::State& state)
{
    const auto executor = ThreadPoolExecutor::create(static_cast<size_t>(state.range(0)));
    constexpr int Parents = 100;
    constexpr int ChildrenPerParent = TasksPerIteration / Parents;

    for (auto _ : state) {
        std::atomic<int> done{0};
        for (int parent = 0; parent < Parents; ++parent) {
            Task::post(executor.get(), [&executor, &done] {
                for (int child = 0; child < ChildrenPerParent; ++child) {
                    Task::post(executor.get(), [&done] { done.fetch_add(1, std::memory_order_relaxed); });
                }
            });
        }
        waitUntil(done, Parents * ChildrenPerParent);
    }
    state.SetItemsProcessed(state.iterations() * Parents * ChildrenPerParent);
}
BENCHMARK(ThreadPool_TasksQueueTasks)->Arg(4)->Arg(16)->UseRealTime();

/// Chains and handles
/// ================================================================================================

// A single task with a handle: queue it and wait for its result (round-trip latency).
void Handle_RoundTrip(benchmark::State& state)
{
    const auto executor = ThreadPoolExecutor::create(4);

    for (auto _ : state) {
        auto handle = Task::run(executor.get(), [] { return 1; }).submit();
        benchmark::DoNotOptimize(handle.get());
    }
}
BENCHMARK(Handle_RoundTrip)->UseRealTime();

// A three-step chain passing results along, waited for through its handle.
void Chain_ThreeSteps(benchmark::State& state)
{
    const auto executor = ThreadPoolExecutor::create(4);

    for (auto _ : state) {
        auto handle = Task::run(executor.get(), [] { return 1; }).then(executor.get(), [](const int value) {
            return value + 1;
        }).then(executor.get(), [](const int value) {
            return value * 2;
        }).submit();
        benchmark::DoNotOptimize(handle.get());
    }
}
BENCHMARK(Chain_ThreeSteps)->UseRealTime();

/// Executor driven by the caller
/// ================================================================================================

// Queue many tasks on the caller-driven executor, then run them all on this thread.
void Manual_QueueAndRun(benchmark::State& state)
{
    const auto executor = ManualExecutor::create();
    int counter = 0;

    for (auto _ : state) {
        for (int i = 0; i < TasksPerIteration; ++i) {
            Task::post(executor.get(), [&counter] { ++counter; });
        }
        executor->runPending(TasksPerIteration);
    }
    benchmark::DoNotOptimize(counter);
    state.SetItemsProcessed(state.iterations() * TasksPerIteration);
}
BENCHMARK(Manual_QueueAndRun);

// Posted functions whose token is already stopped: measures the cost of skipping them.
void Manual_CanceledTasks(benchmark::State& state)
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    source.request_stop();
    int counter = 0;

    for (auto _ : state) {
        for (int i = 0; i < TasksPerIteration; ++i) {
            Task::post(executor.get(), source.get_token(), [&counter] { ++counter; });
        }
        executor->runPending(TasksPerIteration);
    }
    benchmark::DoNotOptimize(counter);
    state.SetItemsProcessed(state.iterations() * TasksPerIteration);
}
BENCHMARK(Manual_CanceledTasks);

// Chains whose token is already stopped: measures delivering the cancellation (to their handles).
void Manual_CanceledChains(benchmark::State& state)
{
    const auto executor = ManualExecutor::create();
    std::stop_source source;
    source.request_stop();
    int counter = 0;

    for (auto _ : state) {
        for (int i = 0; i < TasksPerIteration; ++i) {
            Task::run(executor.get(), source.get_token(), [&counter] { ++counter; }).submit();
        }
        executor->runPending(TasksPerIteration);
    }
    benchmark::DoNotOptimize(counter);
    state.SetItemsProcessed(state.iterations() * TasksPerIteration);
}
BENCHMARK(Manual_CanceledChains);

/// Delayed tasks
/// ================================================================================================

// Many tasks with a tiny delay: measures the timer queue's insert and dispatch cost.
void Timer_DelayedTasks(benchmark::State& state)
{
    const auto executor = ThreadPoolExecutor::create(4);
    constexpr int DelayedTasks = 1'000;

    for (auto _ : state) {
        std::atomic<int> done{0};
        for (int i = 0; i < DelayedTasks; ++i) {
            Task::run(executor.get(), std::chrono::microseconds{1}, [&done] { done.fetch_add(1, std::memory_order_relaxed); }).submit();
        }
        waitUntil(done, DelayedTasks);
    }
    state.SetItemsProcessed(state.iterations() * DelayedTasks);
}
BENCHMARK(Timer_DelayedTasks)->UseRealTime();

BENCHMARK_MAIN();
