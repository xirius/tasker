#include "vanilo/tasker/Tasker.h"

#include <vanilo/tasker/DefaultLocalThreadExecutor.h>
#include <vanilo/tasker/DefaultThreadPoolExecutor.h>
#include <vanilo/tasker/Scheduler.h>

#include <iostream>
#include <mutex>
#include <utility>

using namespace vanilo::tasker;

/// Unhandled exceptions
/// ================================================================================================

namespace {
    void writeToStandardError(const std::exception_ptr& exception)
    {
        try {
            std::rethrow_exception(exception);
        }
        catch (const std::exception& ex) {
            std::cerr << "vanilo::tasker: unhandled exception in a task: " << ex.what() << '\n';
        }
        catch (...) {
            std::cerr << "vanilo::tasker: unhandled non-standard exception in a task\n";
        }
    }

    struct UnhandledExceptionState
    {
        std::mutex mutex;
        UnhandledExceptionHandler handler = writeToStandardError;
    };

    // Function-local static, so it is initialised on first use regardless of static initialisation order.
    UnhandledExceptionState& unhandledExceptionState()
    {
        static UnhandledExceptionState state;
        return state;
    }
} // namespace

UnhandledExceptionHandler vanilo::tasker::setUnhandledExceptionHandler(UnhandledExceptionHandler handler)
{
    auto& state = unhandledExceptionState();
    std::lock_guard lock{state.mutex};
    return std::exchange(state.handler, std::move(handler));
}

void internal::reportUnhandledException(const std::exception_ptr& exception) noexcept
{
    if (!exception) {
        return;
    }

    try {
        std::rethrow_exception(exception);
    }
    catch (const concurrent::OperationCanceledException&) {
        return; // A cancellation is not an error
    }
    catch (...) { // NOSONAR
        // Any other exception is reported below
    }

    UnhandledExceptionHandler handler;
    {
        auto& state = unhandledExceptionState();
        std::lock_guard lock{state.mutex};
        handler = state.handler;
    }

    if (!handler) {
        return;
    }

    try {
        handler(exception);
    }
    catch (...) { // NOSONAR
        // A throwing handler must not take down the executor that reported the exception
    }
}

void internal::cancelTask(Task& task) noexcept
{
    try {
        task.cancel();
        task.run();
    }
    catch (...) {
        reportUnhandledException(std::current_exception());
    }
}

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

namespace {
    // The scheduler for delayed and periodic tasks. Created on first use, so its thread doesn't start when the
    // library is loaded and it is never used before it is initialised (static initialisation order).
    TaskScheduler* defaultScheduler()
    {
        static const auto scheduler = TaskScheduler::create();
        return scheduler.get();
    }
} // namespace

std::unique_ptr<internal::ChainableTask> internal::TaskHelper::convertTask(
    std::unique_ptr<ChainableTask> task, const steady_clock::duration delay)
{
    auto scheduledTask = ScheduledTask::create(defaultScheduler(), steady_clock::now() + delay);
    scheduledTask->setToken(task->getToken());
    scheduledTask->setNext(std::move(task));
    return scheduledTask;
}

std::unique_ptr<internal::ChainableTask> internal::TaskHelper::convertTask(
    std::unique_ptr<ChainableTask> task, const steady_clock::duration delay, const steady_clock::duration interval)
{
    auto scheduledTask = ScheduledTask::create(defaultScheduler(), steady_clock::now() + delay, interval);
    scheduledTask->setToken(task->getToken());
    scheduledTask->setNext(std::move(task));
    return scheduledTask;
}