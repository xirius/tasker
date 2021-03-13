#include <vanilo/concurrent/ConcurrentQueue.h>
#include <vanilo/tasker/DefaultLocalThreadExecutor.h>
#include <vanilo/tasker/DefaultThreadPoolExecutor.h>
#include <vanilo/tasker/Tasker.h>

#include <queue>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

/// TaskExecutor implementation
/// ============================================================================================

size_t TaskExecutor::DefaultThreadNumber = !std::thread::hardware_concurrency() ? 1 : std::thread::hardware_concurrency();

/// LocalThreadExecutor
/// ============================================================================

std::unique_ptr<LocalThreadExecutor> LocalThreadExecutor::create()
{
    return std::make_unique<DefaultLocalThreadExecutor>();
}

/// ThreadPoolExecutor
/// ============================================================================================

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create()
{
    return std::make_unique<DefaultThreadPoolExecutor>(TaskExecutor::DefaultThreadNumber);
}

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create(size_t numThreads)
{
    return std::make_unique<DefaultThreadPoolExecutor>(numThreads);
}
