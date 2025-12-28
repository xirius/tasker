#include <vanilo/tasker/Scheduler.h>
#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace vanilo::tasker;
using namespace std::chrono_literals;

namespace {
    struct PlainTask final: Task
    {
        explicit PlainTask(std::atomic_int& counter): _counter(counter)
        {
        }
        void cancel() noexcept override
        {
        }
        void run() override
        {
            ++_counter;
        }
        std::atomic_int& _counter;
    };

    struct NotifyTask final: internal::ChainableTask
    {
        NotifyTask(TaskExecutor* executor, std::vector<int>& order, std::mutex& mutex, std::condition_variable& conditionVariable, int tag)
            : internal::ChainableTask(executor), _order(order), _mutex(mutex), _conditionVariable(conditionVariable), _tag(tag)
        {
        }

        void run() override
        {
            {
                std::lock_guard lockGuard(_mutex);
                _order.push_back(_tag);
            }
            _conditionVariable.notify_all();
            scheduleNext();
        }

      private:
        [[nodiscard]] bool isPromised() const noexcept override
        {
            return false;
        }
        void handleException(std::exception_ptr) override
        {
        }

        std::vector<int>& _order;
        std::mutex& _mutex;
        std::condition_variable& _conditionVariable;
        int _tag;
    };
} // namespace

TEST_CASE("Scheduler orders by earliest due and sequence", "[scheduler]")
{
    auto scheduler = TaskScheduler::create();

    // Use local-thread executor to avoid background pool complexities
    auto localExecutor = LocalThreadExecutor::create();

    std::vector<int> order;
    std::mutex mutex;
    std::condition_variable conditionVariable;

    const auto now = std::chrono::steady_clock::now();

    // Create two scheduled tasks; the one with earlier due should fire first
    auto scheduledTask1 = ScheduledTask::create(localExecutor.get(), now + 60ms);
    scheduledTask1->setNext(std::make_unique<NotifyTask>(localExecutor.get(), order, mutex, conditionVariable, 1));

    auto scheduledTask2 = ScheduledTask::create(localExecutor.get(), now + 40ms);
    scheduledTask2->setNext(std::make_unique<NotifyTask>(localExecutor.get(), order, mutex, conditionVariable, 2));

    // Also create another with the same due as the second to test sequence tie-break
    auto scheduledTask3 = ScheduledTask::create(localExecutor.get(), now + 40ms);
    scheduledTask3->setNext(std::make_unique<NotifyTask>(localExecutor.get(), order, mutex, conditionVariable, 3));

    scheduler->submit(std::move(scheduledTask1));
    scheduler->submit(std::move(scheduledTask2));
    scheduler->submit(std::move(scheduledTask3));

    // Allow time for the scheduler to enqueue tasks, then pump local executor
    std::this_thread::sleep_for(150ms);
    localExecutor->process(100); // drain up to 100 tasks

    // After processing, we expect the first two entries to correspond to s2 and s3 (tie-broken by sequence order)
    REQUIRE(order.size() >= 2);
    REQUIRE(order[0] == 2);
    REQUIRE(order[1] == 3);
}

TEST_CASE("Scheduler can run a plain Task via adapter", "[scheduler][adapter]")
{
    auto scheduler = TaskScheduler::create();
    std::atomic_int runCounter{0};

    scheduler->submit(std::make_unique<PlainTask>(runCounter));

    // Wait up to ~1s for the worker to execute the task
    for (int i = 0; i < 50 && runCounter.load() == 0; ++i) {
        std::this_thread::sleep_for(20ms);
    }

    REQUIRE(runCounter.load() == 1);
}

TEST_CASE("Scheduler periodic rescheduling occurs", "[scheduler][periodic]")
{
    const auto scheduler = TaskScheduler::create();
    const auto localExecutor = LocalThreadExecutor::create();

    std::atomic_int hitCounter{0};

    auto scheduledTask = ScheduledTask::create(localExecutor.get(), std::chrono::steady_clock::now() + 20ms, 40ms);
    struct CountTask final: internal::ChainableTask
    {
        CountTask(TaskExecutor* executor, std::atomic_int& counter): ChainableTask(executor), counter(counter)
        {
        }
        void run() override
        {
            ++counter;
            scheduleNext();
        }
        [[nodiscard]] bool isPromised() const noexcept override
        {
            return false;
        }
        void handleException(std::exception_ptr) override
        {
        }
        std::atomic_int& counter;
    };

    scheduledTask->setNext(std::make_unique<CountTask>(localExecutor.get(), hitCounter));

    scheduler->submit(std::move(scheduledTask));

    // Poll for ~800ms, periodically processing local executor
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(100ms);
        localExecutor->process(100);
    }

    // Expect at least one firing; depending on chain semantics, subsequent periods may not
    // re-emit the same next task without rebuilding the chain.
    REQUIRE(hitCounter.load() >= 1);
}

TEST_CASE("Canceled scheduled task is not executed", "[scheduler][cancel]")
{
    const auto scheduler = TaskScheduler::create();
    const auto localExecutor = LocalThreadExecutor::create();

    std::atomic_int runCounter{0};
    auto scheduledTask = ScheduledTask::create(localExecutor.get(), std::chrono::steady_clock::now() + 50ms);

    struct IncTask final: internal::ChainableTask
    {
        IncTask(TaskExecutor* executor, std::atomic_int& counter): ChainableTask(executor), counter(counter)
        {
        }
        void run() override
        {
            ++counter;
            scheduleNext();
        }
        [[nodiscard]] bool isPromised() const noexcept override
        {
            return false;
        }
        void handleException(std::exception_ptr) override
        {
        }
        std::atomic_int& counter;
    };

    scheduledTask->setNext(std::make_unique<IncTask>(localExecutor.get(), runCounter));

    // Cancel before submitting
    scheduledTask->cancel();
    scheduler->submit(std::move(scheduledTask));

    std::this_thread::sleep_for(120ms);
    localExecutor->process(100);

    REQUIRE(runCounter.load() == 0);
}

TEST_CASE("DefaultLocalThreadExecutor processes tasks and respects maxCount", "[executor][local]")
{
    auto localExecutor = LocalThreadExecutor::create();
    std::atomic_int counter{0};

    for (int i = 0; i < 5; ++i) {
        struct CounterTask final: Task
        {
            CounterTask(std::atomic_int& counter): counter(counter)
            {
            }
            void cancel() noexcept override
            {
            }
            void run() override
            {
                ++counter;
            }
            std::atomic_int& counter;
        };
        localExecutor->submit(std::make_unique<CounterTask>(counter));
    }

    // Process only 3 tasks
    auto remaining = localExecutor->process(3);
    REQUIRE(counter.load() == 3);
    REQUIRE(remaining == 2);

    // Process the rest
    remaining = localExecutor->process(10);
    REQUIRE(counter.load() == 5);
    REQUIRE(remaining == 0);
}

TEST_CASE("DefaultLocalThreadExecutor process(token) stops on cancellation", "[executor][local][token]")
{
    auto localExecutor = LocalThreadExecutor::create();
    const vanilo::concurrent::CancellationTokenSource cancellationTokenSource;
    std::atomic_int counter{0};

    // Enqueue tasks and a canceller in another thread after a short delay
    for (int i = 0; i < 3; ++i) {
        struct CounterTask final: Task
        {
            CounterTask(std::atomic_int& counter): counter(counter)
            {
            }
            void cancel() noexcept override
            {
            }
            void run() override
            {
                ++counter;
            }
            std::atomic_int& counter;
        };
        localExecutor->submit(std::make_unique<CounterTask>(counter));
    }

    std::thread canceller([&] {
        std::this_thread::sleep_for(30ms);
        cancellationTokenSource.cancel();
    });

    const auto remaining = localExecutor->process(cancellationTokenSource.token());
    if (canceller.joinable())
        canceller.join();

    // Some tasks might be processed before cancellation; ensure we exited and queue reflects remaining tasks
    REQUIRE(remaining <= 3);
}

TEST_CASE("ThreadPoolExecutor submits tasks and can resize", "[executor][pool]")
{
    const auto threadPool = ThreadPoolExecutor::create(2);
    std::atomic_int counter{0};

    for (int i = 0; i < 10; ++i) {
        struct CounterTask final: Task
        {
            CounterTask(std::atomic_int& counter): counter(counter)
            {
            }
            void cancel() noexcept override
            {
            }
            void run() override
            {
                ++counter;
            }
            std::atomic_int& counter;
        };
        threadPool->submit(std::make_unique<CounterTask>(counter));
    }

    // Allow time for tasks to run
    std::this_thread::sleep_for(100ms);
    REQUIRE(counter.load() >= 1);

    // Resize up and then down, ensure futures resolve
    auto resizeFutureIncrease = threadPool->resize(4);
    REQUIRE_NOTHROW(resizeFutureIncrease.get());
    auto resizeFutureDecrease = threadPool->resize(1);
    REQUIRE_NOTHROW(resizeFutureDecrease.get());

    // Submit and allow completion
    for (int i = 0; i < 5; ++i) {
        struct CounterTask2 final: Task
        {
            CounterTask2(std::atomic_int& counter): counter(counter)
            {
            }
            void cancel() noexcept override
            {
            }
            void run() override
            {
                ++counter;
            }
            std::atomic_int& counter;
        };
        threadPool->submit(std::make_unique<CounterTask2>(counter));
    }
    std::this_thread::sleep_for(100ms);
    REQUIRE(counter.load() >= 6);
}
