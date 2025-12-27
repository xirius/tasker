#include "vanilo/tasker/Scheduler.h"

#include <memory>

using namespace vanilo::tasker;

/// DelayedTask
/// ============================================================================================

DelayedTask::DelayedTask(TaskExecutor* executor, const std::chrono::steady_clock::time_point due): ChainableTask{executor}, _due{due}
{
}

void DelayedTask::run()
{
    scheduleNext();
}

[[nodiscard]] bool DelayedTask::isPromised() const noexcept
{
    return false;
}

void DelayedTask::handleException(std::exception_ptr exPtr)
{
    // It is not supposed to throw
}

/// Task scheduler
/// ============================================================================================

std::unique_ptr<TaskScheduler> TaskScheduler::create()
{
    return std::make_unique<TaskScheduler>();
}

TaskScheduler::TaskScheduler(): _worker([this] { worker(); })
{
}

TaskScheduler::~TaskScheduler()
{
    _stop.store(true, std::memory_order_relaxed);
    _condition.notify_one();

    std::cout << "Going to destroy ..." << std::endl;

    if (_worker.joinable()) {
        _worker.join();
        std::cout << "Destroyed" << std::endl;
    }
}

size_t TaskScheduler::count() const
{
    return _queue.size();
}

void TaskScheduler::submit(std::unique_ptr<Task> task)
{
    //_queue.emplace(std::move(task));
}

void TaskScheduler::worker()
{
    try {
        std::cout << "XXXXXXXXXXXXXXXXXXXX" << std::endl;
    }
    catch (...) {
        // Ignore
    }
}
