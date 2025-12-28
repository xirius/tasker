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

/// TaskHelper
/// ========================================================================================

std::unique_ptr<internal::ChainableTask> internal::TaskHelper::convertTask(
    std::unique_ptr<ChainableTask> task, const steady_clock::duration delay)
{
    static auto scheduler = TaskScheduler::create();
    auto scheduledTask = ScheduledTask::create(scheduler.get(), steady_clock::now() + delay);
    scheduledTask->setNext(std::move(task));
    return scheduledTask;
}

std::unique_ptr<internal::ChainableTask> internal::TaskHelper::convertTask(
    std::unique_ptr<ChainableTask> task, const steady_clock::duration delay, const steady_clock::duration interval)
{
    static auto scheduler = TaskScheduler::create();
    auto scheduledTask = ScheduledTask::create(scheduler.get(), steady_clock::now() + delay, interval);
    scheduledTask->setNext(std::move(task));
    return scheduledTask;
}