#include "vanilo/tasker/DefaultLocalThreadExecutor.h"

using namespace vanilo::tasker;

/// Helper methods
/// ================================================================================================

inline void executeTask(Task* task)
{
    try {
        task->run();
    }
    catch (...) {
        internal::reportUnhandledException(std::current_exception());
    }
}

/// DefaultLocalThreadExecutor implementation
/// ================================================================================================

DefaultLocalThreadExecutor::~DefaultLocalThreadExecutor()
{
    // Ensure that no more tasks can be scheduled on this executor from now on.
    getGuard()->terminate();

    // The pending tasks will never run: cancel them, so their futures and onException handlers know.
    for (const auto& task : _queue.close()) {
        internal::cancelTask(*task);
    }
}

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
        executeTask(task.get());

        if (++counter >= maxCount)
            break;
    }

    return _queue.size();
}

size_t DefaultLocalThreadExecutor::process(const CancellationToken& token)
{
    std::unique_ptr<Task> task;

    while (_queue.waitDequeue(token, task)) {
        executeTask(task.get());
    }

    return _queue.size();
}

void DefaultLocalThreadExecutor::submit(std::unique_ptr<Task> task)
{
    if (!_queue.enqueue(std::move(task))) { // The queue is closed: the executor is being destroyed
        internal::cancelTask(*task);
    }
}