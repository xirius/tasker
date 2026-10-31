#include <vanilo/concurrent/RingBuffer.h>
#include <vanilo/tasker/ExecutorCore.h>
#include <vanilo/tasker/TaskExecutor.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace vanilo::tasker::detail;

namespace {

    /**
     * Tells the CPU that this thread is spinning.
     */
    void cpuRelax() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
        asm volatile("yield");
#else
        std::this_thread::yield();
#endif
    }

    /**
     * The queue of one worker. Its size is mirrored in an atomic, so idle workers look for work without locking.
     * The size is stored and loaded with sequential consistency, like the pool's sleeper count: a producer either
     * sees a sleeping worker, or the worker sees the producer's task (see PoolCore::work).
     */
    struct alignas(64) WorkerQueue
    {
        std::mutex mutex;
        RingBuffer<Job> jobs;
        std::atomic<std::size_t> size{0};
        bool closed{false};

        bool push(Job& job)
        {
            std::lock_guard lock{mutex};
            if (closed) {
                return false;
            }
            jobs.emplaceBack(std::move(job));
            size.store(jobs.size(), std::memory_order_seq_cst);
            return true;
        }

        bool pop(Job& out)
        {
            if (size.load(std::memory_order_seq_cst) == 0) {
                return false;
            }

            std::lock_guard lock{mutex};
            if (jobs.empty()) {
                return false;
            }
            out = jobs.popFront();
            size.store(jobs.size(), std::memory_order_relaxed);
            return true;
        }
    };

    class PoolCore;

    // The pool and queue of the current thread, if it is a pool thread: tasks it queues go to its own queue.
    thread_local const PoolCore* currentPool = nullptr;
    thread_local std::size_t currentQueue = 0;

    // Spreads the tasks queued from outside the pool over the queues; per thread, so producers don't contend.
    thread_local std::size_t nextQueue = std::hash<std::thread::id>{}(std::this_thread::get_id());

    /// ThreadPoolExecutor core
    /// ============================================================================================

    /**
     * A work-stealing pool: one queue per thread (threads share queues beyond the count it was created with).
     * Idle threads take work from the other queues, spin a little, then sleep until a task is queued. Queueing
     * only wakes a thread when one sleeps, and never under a lock.
     */
    class PoolCore final: public ExecutorCore, public std::enable_shared_from_this<PoolCore>
    {
      public:
        explicit PoolCore(const std::size_t queueCount)
            : _queues{std::make_unique<WorkerQueue[]>(std::max<std::size_t>(queueCount, 1))},
              _queueCount{std::max<std::size_t>(queueCount, 1)}
        {
        }

        bool tryEnqueue(Job& job) override
        {
            std::size_t index;
            if (currentPool == this) {
                index = currentQueue;
            }
            else {
                const auto active = std::clamp<std::size_t>(_threadCount.load(std::memory_order_relaxed), 1, _queueCount);
                index = nextQueue++ % active;
            }

            if (!_queues[index].push(job)) {
                return false;
            }
            wakeOne();
            return true;
        }

        [[nodiscard]] std::size_t pendingCount() const noexcept override
        {
            std::size_t count = 0;
            for (std::size_t i = 0; i < _queueCount; ++i) {
                count += _queues[i].size.load(std::memory_order_relaxed);
            }
            return count;
        }

        std::future<void> resize(const std::size_t threadCount)
        {
            std::lock_guard lock{_mutex};
            joinExited();

            auto batch = std::make_shared<RetireBatch>();
            auto future = batch->done.get_future();

            while (_workers.size() < threadCount) {
                // The thread shares its core and worker: a pool destroyed by its own task detaches that thread,
                // which then still uses both.
                auto worker = std::make_shared<Worker>();
                worker->queue = _workers.size() % _queueCount;
                _usedQueues.store(std::max(_usedQueues.load(std::memory_order_relaxed), worker->queue + 1), std::memory_order_release);
                worker->thread = std::thread{[self = shared_from_this(), worker] { self->work(*worker); }};
                worker->id = worker->thread.get_id();
                _workers.push_back(std::move(worker));
            }

            if (_workers.size() > threadCount) {
                batch->remaining = static_cast<int>(_workers.size() - threadCount);
                while (_workers.size() > threadCount) {
                    auto& worker = _workers.back();
                    worker->batch = batch;
                    worker->retiring.store(true, std::memory_order_release);
                    _retired.push_back(std::move(worker));
                    _workers.pop_back();
                }
                wakeAll(); // the retiring threads may sleep
            }
            else {
                batch->done.set_value();
            }

            _threadCount.store(_workers.size(), std::memory_order_relaxed);
            return future;
        }

        [[nodiscard]] bool containsThread(const std::thread::id threadId) const
        {
            std::lock_guard lock{_mutex};
            return std::any_of(_workers.begin(), _workers.end(), [threadId](const auto& worker) { return worker->id == threadId; });
        }

        [[nodiscard]] std::size_t threadCount() const noexcept
        {
            return _threadCount.load(std::memory_order_relaxed);
        }

        [[nodiscard]] std::vector<std::thread::id> threadIds() const
        {
            std::lock_guard lock{_mutex};
            std::vector<std::thread::id> ids;
            ids.reserve(_workers.size());
            for (const auto& worker : _workers) {
                ids.push_back(worker->id);
            }
            return ids;
        }

        void shutdown() noexcept
        {
            std::vector<std::shared_ptr<Worker>> workers;
            {
                std::lock_guard lock{_mutex};
                workers = std::move(_workers);
                workers.insert(workers.end(), _retired.begin(), _retired.end());
                _workers.clear();
                _retired.clear();
                _threadCount.store(0, std::memory_order_relaxed);
            }

            _stopping.store(true, std::memory_order_release);
            wakeAll();

            // The threads finish their current task. A pool destroyed by one of its own tasks can't join that
            // thread: it is detached, and keeps the core and its worker alive until it ends.
            for (auto& worker : workers) {
                if (worker->thread.get_id() == std::this_thread::get_id()) {
                    worker->thread.detach();
                }
                else if (worker->thread.joinable()) {
                    worker->thread.join();
                }
            }

            // The pending jobs will never run: cancel them (outside the locks), so their chains know.
            RingBuffer<Job> pending;
            for (std::size_t i = 0; i < _queueCount; ++i) {
                auto& queue = _queues[i];
                std::lock_guard lock{queue.mutex};
                queue.closed = true;
                while (!queue.jobs.empty()) {
                    pending.emplaceBack(queue.jobs.popFront());
                }
                queue.size.store(0, std::memory_order_relaxed);
            }
            while (!pending.empty()) {
                pending.popFront().cancel();
            }
        }

      private:
        static constexpr int SpinCount = 64;

        struct RetireBatch
        {
            std::atomic<int> remaining{0};
            std::promise<void> done;
        };

        struct Worker
        {
            std::thread thread;
            std::thread::id id;
            std::size_t queue{0};
            std::atomic<bool> retiring{false};
            std::shared_ptr<RetireBatch> batch; // set when the worker is removed by resize()
            std::atomic<bool> exited{false};
        };

        [[nodiscard]] bool shouldExit(const Worker& worker) const noexcept
        {
            return _stopping.load(std::memory_order_acquire) || worker.retiring.load(std::memory_order_acquire);
        }

        /**
         * Takes a job from the worker's queue, else from the others (oldest first).
         */
        bool takeWork(const std::size_t own, Job& out)
        {
            if (_queues[own].pop(out)) {
                return true;
            }
            const auto used = _usedQueues.load(std::memory_order_acquire);
            for (std::size_t offset = 1; offset < used; ++offset) {
                if (_queues[(own + offset) % used].pop(out)) {
                    return true;
                }
            }
            return false;
        }

        void work(Worker& worker)
        {
            currentPool = this;
            currentQueue = worker.queue;
            Job job;

            while (!shouldExit(worker)) {
                if (takeWork(worker.queue, job)) {
                    runJob(job);
                    continue;
                }

                // Spin a little before sleeping: work often comes in bursts.
                bool found = false;
                for (int spin = 0; spin < SpinCount && !found; ++spin) {
                    cpuRelax();
                    found = takeWork(worker.queue, job);
                }
                if (found) {
                    runJob(job);
                    continue;
                }

                // Announce the sleep, then look once more: a producer either sees the sleeper (and wakes it)
                // or its job is seen here (both sides use sequentially consistent operations).
                _sleepers.fetch_add(1, std::memory_order_seq_cst);
                const auto epoch = _epoch.load(std::memory_order_seq_cst);

                if (takeWork(worker.queue, job)) {
                    _sleepers.fetch_sub(1, std::memory_order_relaxed);
                    runJob(job);
                    continue;
                }
                if (!shouldExit(worker)) {
                    _epoch.wait(epoch, std::memory_order_acquire);
                }
                _sleepers.fetch_sub(1, std::memory_order_relaxed);
            }

            currentPool = nullptr;
            retire(worker);
        }

        void retire(Worker& worker)
        {
            std::shared_ptr<RetireBatch> batch;
            {
                std::lock_guard lock{_mutex};
                batch = std::move(worker.batch);
            }
            if (batch && batch->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                batch->done.set_value();
            }
            worker.exited.store(true, std::memory_order_release);
        }

        void wakeOne() noexcept
        {
            if (_sleepers.load(std::memory_order_seq_cst) > 0) {
                _epoch.fetch_add(1, std::memory_order_release);
                _epoch.notify_one();
            }
        }

        void wakeAll() noexcept
        {
            _epoch.fetch_add(1, std::memory_order_seq_cst);
            _epoch.notify_all();
        }

        // pre: the lock is held
        void joinExited()
        {
            std::erase_if(_retired, [](const auto& worker) {
                if (!worker->exited.load(std::memory_order_acquire)) {
                    return false;
                }
                worker->thread.join(); // it has finished its work: this returns at once
                return true;
            });
        }

        std::unique_ptr<WorkerQueue[]> _queues;
        std::size_t _queueCount;
        std::atomic<std::size_t> _usedQueues{1}; // the queues that ever had a thread (and may hold tasks)
        std::atomic<std::uint32_t> _epoch{0};
        std::atomic<int> _sleepers{0};
        std::atomic<bool> _stopping{false};
        std::atomic<std::size_t> _threadCount{0};

        mutable std::mutex _mutex; // guards the workers
        std::vector<std::shared_ptr<Worker>> _workers;
        std::vector<std::shared_ptr<Worker>> _retired; // removed by resize(), joined later
    };

    PoolCore& poolCore(const TaskExecutor& executor) noexcept
    {
        return static_cast<PoolCore&>(*ExecutorAccess::core(executor));
    }

} // namespace

/// ThreadPoolExecutor
/// ================================================================================================

struct ThreadPoolExecutor::Private
{
};

std::size_t ThreadPoolExecutor::defaultThreadCount() noexcept
{
    return std::max(1u, std::thread::hardware_concurrency());
}

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create()
{
    return create(defaultThreadCount());
}

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create(const std::size_t threadCount)
{
    return std::make_unique<ThreadPoolExecutor>(Private{}, threadCount);
}

ThreadPoolExecutor::ThreadPoolExecutor(Private, const std::size_t threadCount)
    : TaskExecutor{std::make_shared<PoolCore>(std::max(threadCount, defaultThreadCount()))}
{
    try {
        poolCore(*this).resize(threadCount);
    }
    catch (...) {
        poolCore(*this).shutdown();
        throw;
    }
}

ThreadPoolExecutor::~ThreadPoolExecutor()
{
    poolCore(*this).shutdown();
}

std::future<void> ThreadPoolExecutor::resize(const std::size_t threadCount)
{
    return poolCore(*this).resize(threadCount);
}

bool ThreadPoolExecutor::containsThread(const std::thread::id threadId) const
{
    return poolCore(*this).containsThread(threadId);
}

std::size_t ThreadPoolExecutor::threadCount() const noexcept
{
    return poolCore(*this).threadCount();
}

std::vector<std::thread::id> ThreadPoolExecutor::threadIds() const
{
    return poolCore(*this).threadIds();
}
