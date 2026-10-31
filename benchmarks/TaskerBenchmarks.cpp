// Benchmarks of the task runner. Each benchmark describes what it measures; run them with
// cmake --workflow ... or build the benchmark preset and start bin/Vanilo.benchmarks.

#include <vanilo/tasker/Tasker.h>

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace vanilo::concurrent;
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
            Task::run(executor.get(), [&done] { done.fetch_add(1, std::memory_order_relaxed); });
        }
        waitUntil(done, TasksPerIteration);
    }
    state.SetItemsProcessed(state.iterations() * TasksPerIteration);
}
BENCHMARK(ThreadPool_SingleProducer)->Arg(1)->Arg(4)->Arg(16)->UseRealTime();

// Several threads queue tasks at the same time; measures contention on the pool's queue.
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
                    Task::run(executor.get(), [&done] { done.fetch_add(1, std::memory_order_relaxed); });
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
            Task::run(executor.get(), [&executor, &done] {
                for (int child = 0; child < ChildrenPerParent; ++child) {
                    Task::run(executor.get(), [&done] { done.fetch_add(1, std::memory_order_relaxed); });
                }
            });
        }
        waitUntil(done, Parents * ChildrenPerParent);
    }
    state.SetItemsProcessed(state.iterations() * Parents * ChildrenPerParent);
}
BENCHMARK(ThreadPool_TasksQueueTasks)->Arg(4)->Arg(16)->UseRealTime();

/// Chains and futures
/// ================================================================================================

// A single task with a future: queue it and wait for its result (round-trip latency).
void Future_RoundTrip(benchmark::State& state)
{
    const auto executor = ThreadPoolExecutor::create(4);

    for (auto _ : state) {
        auto future = Task::run(executor.get(), [] { return 1; }).getFuture();
        benchmark::DoNotOptimize(future.get());
    }
}
BENCHMARK(Future_RoundTrip)->UseRealTime();

// A three-step chain passing results along, waited for through a future.
void Chain_ThreeSteps(benchmark::State& state)
{
    const auto executor = ThreadPoolExecutor::create(4);

    for (auto _ : state) {
        auto future = Task::run(executor.get(), [] { return 1; })
                          .then(executor.get(), [](const int value) { return value + 1; })
                          .then(executor.get(), [](const int value) { return value * 2; })
                          .getFuture();
        benchmark::DoNotOptimize(future.get());
    }
}
BENCHMARK(Chain_ThreeSteps)->UseRealTime();

/// Executor driven by the caller
/// ================================================================================================

// Queue many tasks on the caller-driven executor, then run them all on this thread.
void Manual_QueueAndRun(benchmark::State& state)
{
    const auto executor = LocalThreadExecutor::create();
    int counter = 0;

    for (auto _ : state) {
        for (int i = 0; i < TasksPerIteration; ++i) {
            Task::run(executor.get(), [&counter] { ++counter; });
        }
        executor->process(TasksPerIteration);
    }
    benchmark::DoNotOptimize(counter);
    state.SetItemsProcessed(state.iterations() * TasksPerIteration);
}
BENCHMARK(Manual_QueueAndRun);

// Tasks whose token is already canceled: measures the cost of the cancellation path.
void Manual_CanceledTasks(benchmark::State& state)
{
    const auto executor = LocalThreadExecutor::create();
    CancellationTokenSource source;
    source.cancel();
    int counter = 0;

    for (auto _ : state) {
        for (int i = 0; i < TasksPerIteration; ++i) {
            Task::run(executor.get(), source.token(), [&counter] { ++counter; });
        }
        executor->process(TasksPerIteration);
    }
    benchmark::DoNotOptimize(counter);
    state.SetItemsProcessed(state.iterations() * TasksPerIteration);
}
BENCHMARK(Manual_CanceledTasks);

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
            Task::run(executor.get(), CancellationToken{}, std::chrono::microseconds{1}, [&done] {
                done.fetch_add(1, std::memory_order_relaxed);
            });
        }
        waitUntil(done, DelayedTasks);
    }
    state.SetItemsProcessed(state.iterations() * DelayedTasks);
}
BENCHMARK(Timer_DelayedTasks)->UseRealTime();

BENCHMARK_MAIN();
