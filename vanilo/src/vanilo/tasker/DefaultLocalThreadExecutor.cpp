#include <vanilo/tasker/DefaultLocalThreadExecutor.h>

using namespace vanilo::tasker;

/// DefaultThreadPoolExecutor implementation
/// ================================================================================================

size_t DefaultLocalThreadExecutor::count() const
{
    return _queueSize;
}

size_t DefaultLocalThreadExecutor::process(size_t maxCount)
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
            TRACE("An unhandled exception occurred during execution of the task. Message: %s", ex.what());
        }
        catch (...) {
            TRACE("An unhandled exception occurred during execution of the task!");
        }

        if (++counter >= maxCount)
            break;
    }

    return queueSize;
}

void DefaultLocalThreadExecutor::submit(std::unique_ptr<Task> task)
{
    std::lock_guard<std::mutex> lock{_mutex};
    _queue.emplace(std::move(task));
    ++_queueSize;
}

//! Private members
std::unique_ptr<Task> DefaultLocalThreadExecutor::nextTask(size_t& queueSize)
{
    std::lock_guard<std::mutex> lock{_mutex};

    if (_queue.empty())
        return nullptr;

    auto task = std::move(_queue.front());
    _queue.pop();

    queueSize = --_queueSize;
    return task;
}
