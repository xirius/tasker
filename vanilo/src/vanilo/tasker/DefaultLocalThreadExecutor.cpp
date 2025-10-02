#include <vanilo/tasker/DefaultLocalThreadExecutor.h>

using namespace vanilo::tasker;

/// Helper methods
/// ================================================================================================

inline void executeTask(const std::unique_ptr<Task>& task)
{
    try {
        task->run();
    }
    catch (const std::exception& ex) {
        TRACE("An unhandled exception occurred during execution of the task. Message: %s", ex.what());
    }
    catch (...) {
        TRACE("An unhandled exception occurred during execution of the task!");
    }
}

/// DefaultThreadPoolExecutor implementation
/// ================================================================================================

size_t DefaultLocalThreadExecutor::count() const
{
    return _queue.size();
}

size_t DefaultLocalThreadExecutor::process(const size_t maxCount)
{
    if (maxCount == 0) {
        return _queue.size();
    }

    size_t counter = 0;
    std::unique_ptr<Task> task;

    while (_queue.tryDequeue(task)) {
        executeTask(task);

        if (++counter >= maxCount)
            break;
    }

    return _queue.size();
}

size_t DefaultLocalThreadExecutor::process(CancellationToken& token)
{
    std::unique_ptr<Task> task;

    while (_queue.waitDequeue(token, task)) {
        executeTask(task);
    }

    return _queue.size();
}

void DefaultLocalThreadExecutor::submit(std::unique_ptr<Task> task)
{
    _queue.enqueue(std::move(task));
}