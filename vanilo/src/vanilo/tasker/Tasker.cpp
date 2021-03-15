#include <vanilo/tasker/DefaultLocalThreadExecutor.h>
#include <vanilo/tasker/DefaultThreadPoolExecutor.h>
#include <vanilo/tasker/Tasker.h>

using namespace vanilo::tasker;

/// LocalThreadExecutor
/// ============================================================================

std::unique_ptr<LocalThreadExecutor> LocalThreadExecutor::create()
{
    return std::make_unique<DefaultLocalThreadExecutor>();
}

/// ThreadPoolExecutor
/// ============================================================================================

size_t ThreadPoolExecutor::DefaultThreadNumber = !std::thread::hardware_concurrency() ? 1 : std::thread::hardware_concurrency();

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create()
{
    return std::make_unique<DefaultThreadPoolExecutor>(ThreadPoolExecutor::DefaultThreadNumber);
}

std::unique_ptr<ThreadPoolExecutor> ThreadPoolExecutor::create(size_t numThreads)
{
    return std::make_unique<DefaultThreadPoolExecutor>(numThreads);
}
