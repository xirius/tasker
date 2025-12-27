#include "vanilo/tasker/Tasker.h"

#include <vanilo/tasker/DefaultLocalThreadExecutor.h>
#include <vanilo/tasker/DefaultThreadPoolExecutor.h>
#include <vanilo/tasker/Scheduler.h>

using namespace vanilo::tasker;

/// LocalThreadExecutor
/// ================================================================================================

std::unique_ptr<LocalThreadExecutor> LocalThreadExecutor::create()
{
    return std::make_unique<DefaultLocalThreadExecutor>();
}

/// ThreadPoolExecutor
/// ================================================================================================

size_t ThreadPoolExecutor::DefaultThreadNumber = !std::thread::hardware_concurrency() ? 1 : std::thread::hardware_concurrency();

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create()
{
    return std::make_unique<DefaultThreadPoolExecutor>(DefaultThreadNumber);
}

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create(size_t numThreads)
{
    return std::make_unique<DefaultThreadPoolExecutor>(numThreads);
}

/// TaskManager
/// ========================================================================================

std::unique_ptr<internal::ChainableTask> internal::TaskManager::convertTask(
    std::unique_ptr<ChainableTask> task, const steady_clock::duration delay)
{
    static auto scheduler = TaskScheduler::create();
    auto delayedTask = DelayedTask::create(scheduler.get(), steady_clock::now() + delay);
    delayedTask->setNext(std::move(task));
    return delayedTask;
}

/*
std::unique_ptr<internal::ChainableTask> internal::TaskManager::createTask(
    std::unique_ptr<ChainableTask> task, steady_clock::time_point due)
{
    return std::make_unique<ScheduledTask>(nullptr, steady_clock::now() + std::chrono::duration_cast<steady_clock::duration>(delay));
}
*/