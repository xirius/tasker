#include <vanilo/concurrent/ConcurrentQueue.h>
#include <vanilo/tasker/Tasker.h>

#include <queue>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

/// DefaultLocalThreadExecutor
/// ============================================================================

class DefaultLocalThreadExecutor: public LocalThreadExecutor
{
  public:
    [[nodiscard]] size_t count() const override
    {
        return _queueSize;
    }

    size_t process(size_t maxCount) override
    {
        size_t counter   = 0;
        size_t queueSize = 0;

        if (counter >= maxCount)
            return _queueSize.load();

        while (auto task = nextTask(queueSize)) {
            try {
                task->run();
            }
            catch (const std::exception& ex) {
                TRACE("An unhandled exception occurred during task execution. Message: %s", ex.what());
            }
            catch (...) {
                TRACE("An unhandled exception occurred during task execution!");
            }

            if (++counter >= maxCount)
                break;
        }

        return queueSize;
    }

    void submit(std::unique_ptr<Task> task) override
    {
        std::lock_guard<std::mutex> lock{_mutex};
        _queue.emplace(std::move(task));
        ++_queueSize;
    }

  private:
    inline std::unique_ptr<Task> nextTask(size_t& queueSize)
    {
        std::lock_guard<std::mutex> lock{_mutex};

        if (_queue.empty())
            return nullptr;

        auto task = std::move(_queue.front());
        _queue.pop();

        queueSize = --_queueSize;
        return task;
    }

  private:
    std::queue<std::unique_ptr<Task>> _queue;
    std::atomic<size_t> _queueSize{0};
    mutable std::mutex _mutex;
};

/// QueuedTaskExecutor
/// ============================================================================

std::unique_ptr<LocalThreadExecutor> LocalThreadExecutor::create()
{
    return std::make_unique<DefaultLocalThreadExecutor>();
}

/// DefaultThreadPoolExecutor
/// ============================================================================

class DefaultThreadPoolExecutor: public ThreadPoolExecutor
{
  public:
    explicit DefaultThreadPoolExecutor(size_t numThreads)
    {
        try {
            for (auto i = 0u; i < numThreads; i++) {
                _threads.emplace_back(&DefaultThreadPoolExecutor::worker, this);
            }
        }
        catch (...) {
            invalidate();
            TRACE("An unhandled exception occurred during initialisation of the DefaultThreadPoolExecutor!");
            throw;
        }
    }

    ~DefaultThreadPoolExecutor() override
    {
        invalidate();
    }

    [[nodiscard]] size_t count() const override
    {
        return 0;
    }

    void submit(std::unique_ptr<Task> task) override
    {
        _queue.enqueue(std::move(task));
    }

  private:
    void invalidate()
    {
        auto tasks = _queue.invalidate();

        for (auto& task : tasks) {
            task->cancel();
            // Not enough !!!
        }

        for (auto& thread : _threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void worker()
    {
        std::unique_ptr<Task> task;

        while (true) {
            if (!_queue.waitDequeue(task)) {
                return;
            }

            task->run();
        }
    }

    ConcurrentQueue<std::unique_ptr<Task>> _queue;
    std::vector<std::thread> _threads;
};

/// ThreadPoolExecutor
/// ============================================================================================

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create(size_t numThreads)
{
    return std::make_unique<DefaultThreadPoolExecutor>(numThreads);
}
