#include <vanilo/concurrent/ConcurrentQueue.h>
#include <vanilo/tasker/ExecutorCore.h>
#include <vanilo/tasker/Task.h>
#include <vanilo/tasker/TaskExecutor.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace vanilo::tasker::detail;

namespace {

    /**
     * A job running a Task object.
     */
    struct TaskObjectJob
    {
        std::unique_ptr<Task> task;

        void run()
        {
            task->run();
        }

        void cancel() noexcept
        {
            task->cancel();
        }
    };

    constexpr std::size_t BatchSize = 32;

    /// ManualExecutor core
    /// ============================================================================================

    class ManualCore final: public ExecutorCore
    {
      public:
        bool tryEnqueue(Job& job) override
        {
            return _queue.enqueue(std::move(job));
        }

        [[nodiscard]] std::size_t pendingCount() const noexcept override
        {
            return _queue.size();
        }

        std::size_t runPending(const std::size_t maxCount)
        {
            std::array<Job, BatchSize> batch;
            std::size_t ran = 0;

            while (ran < maxCount) {
                const auto count = _queue.tryDequeueBulk(batch.begin(), std::min(BatchSize, maxCount - ran));
                if (count == 0) {
                    break;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    runJob(batch[i]);
                }
                ran += count;
            }

            return _queue.size();
        }

        std::size_t runUntil(const std::stop_token& token)
        {
            std::array<Job, BatchSize> batch;

            while (const auto count = _queue.waitDequeueBulk(token, batch.begin(), BatchSize)) {
                for (std::size_t i = 0; i < count; ++i) {
                    runJob(batch[i]);
                }
            }

            return _queue.size();
        }

        void shutdown() noexcept
        {
            // The pending jobs will never run: cancel them, so their chains know.
            for (auto& job : _queue.close()) {
                job.cancel();
            }
        }

      private:
        ConcurrentQueue<Job> _queue;
    };

    /// ThreadPoolExecutor core
    /// ============================================================================================

    class PoolCore final: public ExecutorCore, public std::enable_shared_from_this<PoolCore>
    {
      public:
        bool tryEnqueue(Job& job) override
        {
            return _queue.enqueue(std::move(job));
        }

        [[nodiscard]] std::size_t pendingCount() const noexcept override
        {
            return _queue.size();
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
                worker->thread = std::thread{[self = shared_from_this(), worker] { self->work(*worker); }};
                worker->id = worker->thread.get_id();
                _workers.push_back(std::move(worker));
            }

            if (_workers.size() > threadCount) {
                batch->remaining = static_cast<int>(_workers.size() - threadCount);
                while (_workers.size() > threadCount) {
                    auto& worker = _workers.back();
                    worker->batch = batch;
                    worker->stop.request_stop();
                    _retired.push_back(std::move(worker));
                    _workers.pop_back();
                }
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
                for (auto& worker : _workers) {
                    worker->stop.request_stop();
                    workers.push_back(std::move(worker));
                }
                for (auto& worker : _retired) {
                    workers.push_back(std::move(worker));
                }
                _workers.clear();
                _retired.clear();
                _threadCount.store(0, std::memory_order_relaxed);
            }

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

            // The pending jobs will never run: cancel them, so their chains know.
            for (auto& job : _queue.close()) {
                job.cancel();
            }
        }

      private:
        struct RetireBatch
        {
            std::atomic<int> remaining{0};
            std::promise<void> done;
        };

        struct Worker
        {
            std::stop_source stop;
            std::thread thread;
            std::thread::id id;
            std::shared_ptr<RetireBatch> batch; // set when the worker is removed by resize()
            std::atomic<bool> exited{false};
        };

        void work(Worker& worker)
        {
            const auto token = worker.stop.get_token();
            Job job;

            while (!token.stop_requested() && _queue.waitDequeue(token, job)) {
                runJob(job);
            }

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

        ConcurrentQueue<Job> _queue;
        mutable std::mutex _mutex;
        std::vector<std::shared_ptr<Worker>> _workers;
        std::vector<std::shared_ptr<Worker>> _retired; // removed by resize(), joined later
        std::atomic<std::size_t> _threadCount{0};
    };

    ManualCore& manualCore(const TaskExecutor& executor) noexcept
    {
        return static_cast<ManualCore&>(*ExecutorAccess::core(executor));
    }

    PoolCore& poolCore(const TaskExecutor& executor) noexcept
    {
        return static_cast<PoolCore&>(*ExecutorAccess::core(executor));
    }

} // namespace

/// TaskExecutor
/// ================================================================================================

TaskExecutor::TaskExecutor(std::shared_ptr<ExecutorCore> core) noexcept: _core{std::move(core)}
{
}

TaskExecutor::~TaskExecutor() = default;

std::size_t TaskExecutor::pendingCount() const noexcept
{
    return _core->pendingCount();
}

void TaskExecutor::submit(std::unique_ptr<Task> task)
{
    if (!task) {
        return;
    }

    auto job = Job::make<TaskObjectJob>(std::move(task));
    if (!_core->tryEnqueue(job)) {
        job.cancel();
    }
}

/// ManualExecutor
/// ================================================================================================

struct ManualExecutor::Private
{
};

std::unique_ptr<ManualExecutor> ManualExecutor::create()
{
    return std::make_unique<ManualExecutor>(Private{});
}

ManualExecutor::ManualExecutor(Private): TaskExecutor{std::make_shared<ManualCore>()}
{
}

ManualExecutor::~ManualExecutor()
{
    manualCore(*this).shutdown();
}

std::size_t ManualExecutor::runPending(const std::size_t maxCount)
{
    return manualCore(*this).runPending(maxCount);
}

std::size_t ManualExecutor::runUntil(const std::stop_token token)
{
    return manualCore(*this).runUntil(token);
}

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

ThreadPoolExecutor::ThreadPoolExecutor(Private, const std::size_t threadCount): TaskExecutor{std::make_shared<PoolCore>()}
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
