#include <vanilo/tasker/Tasker.h>

#include <queue>

using namespace vanilo::tasker;

/// DefaultQueuedTaskExecutor
/// ============================================================================

class DefaultQueuedTaskExecutor: public QueuedTaskExecutor
{
  public:
    size_t count() const override
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
                std::cerr << "An exception occurred during task execution. Message: " << ex.what() << std::endl;
            }

            if (++counter >= maxCount)
                break;
        }

        return queueSize;
    }

    void submit(std::unique_ptr<Task> task) override
    {
        auto guard = std::lock_guard(_mutex);
        _queue.emplace(std::move(task));
        ++_queueSize;
    }

  private:
    inline std::unique_ptr<Task> nextTask(size_t& queueSize)
    {
        auto guard = std::lock_guard(_mutex);

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

std::unique_ptr<QueuedTaskExecutor> QueuedTaskExecutor::create()
{
    return std::make_unique<DefaultQueuedTaskExecutor>();
}
