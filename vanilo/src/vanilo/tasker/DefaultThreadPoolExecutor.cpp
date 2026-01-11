#include "vanilo/tasker/DefaultThreadPoolExecutor.h"

#include <vanilo/core/Tracer.h>

#include <mutex>
#include <vector>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

/// ThreadPool system tasks
/// ================================================================================================

class StopThreadException final: public std::exception
{
  public:
    [[nodiscard]] const char* what() const noexcept override
    {
        return "Stop the thread execution exception!";
    }
};

class StopThreadTask final: public Task
{
  public:
    explicit StopThreadTask(
        std::shared_ptr<std::promise<void>> promise,
        std::shared_ptr<std::atomic_int> counter,
        std::map<std::thread::id, std::thread>& threads,
        std::mutex& mutex)
        : _promise{std::move(promise)}, _counter{std::move(counter)}, _threads{threads}, _mutex{mutex}
    {
    }

    void cancel() noexcept override
    {
        _cancelled.store(true, std::memory_order_release);
    }

    [[noreturn]] void run() override
    {
        if (!_cancelled.load(std::memory_order_acquire)) {
            std::lock_guard lock{_mutex};
            if (const auto it = _threads.find(std::this_thread::get_id()); it != _threads.end()) {
                it->second.detach(); // NOSONAR
                _threads.erase(it);
            }
            // else: thread might have been removed by invalidate() or another resize
        }

        if (--*_counter == 0) {
            // All the scheduled threads that have to be stopped are found
            _promise->set_value();
        }

        throw StopThreadException{};
    }

  private:
    std::shared_ptr<std::promise<void>> _promise;
    std::shared_ptr<std::atomic_int> _counter;
    std::map<std::thread::id, std::thread>& _threads;
    std::atomic_bool _cancelled{false};
    std::mutex& _mutex;
};

/// DefaultThreadPoolExecutor implementation
/// ================================================================================================

DefaultThreadPoolExecutor::DefaultThreadPoolExecutor(const size_t numThreads)
{
    init(numThreads);
}

DefaultThreadPoolExecutor::~DefaultThreadPoolExecutor()
{
    try {
        invalidate();
    }
    catch (...) { // NOSONAR
        // Ignore
    }
}

bool DefaultThreadPoolExecutor::containsThread(const std::thread::id threadId) const
{
    std::lock_guard lock{_mutex};
    return _threads.find(threadId) != _threads.end();
}

size_t DefaultThreadPoolExecutor::count() const
{
    return _queue.size();
}

size_t DefaultThreadPoolExecutor::threadCount() const noexcept
{
    std::lock_guard lock{_mutex};
    return _threads.size();
}

std::vector<std::thread::id> DefaultThreadPoolExecutor::threadIds() const
{
    std::lock_guard lock{_mutex};
    std::vector<std::thread::id> ids;

    ids.reserve(_threads.size());
    for (const auto& [id, thread] : _threads) {
        ids.push_back(id);
    }

    return ids;
}

std::future<void> DefaultThreadPoolExecutor::resize(const size_t numThreads)
{
    std::lock_guard lock{_mutex};
    auto promise = std::make_shared<std::promise<void>>();

    if (_threads.size() < numThreads) {
        for (auto i = _threads.size(); i < numThreads; i++) {
            std::thread thread{&DefaultThreadPoolExecutor::worker, this};
            _threads.emplace(thread.get_id(), std::move(thread));
        }

        promise->set_value();
        return promise->get_future();
    }

    if (_threads.size() > numThreads) {
        auto counter = std::make_shared<std::atomic_int>(_threads.size() - numThreads);

        for (auto i = _threads.size(); i > numThreads; i--) {
            // Use enqueueFront (priority) to ensure we stop threads as soon as possible,
            // preventing starvation if the queue has long-running tasks.
            _queue.enqueueFront(std::make_unique<StopThreadTask>(promise, counter, _threads, _mutex));
        }

        return promise->get_future();
    }

    promise->set_value();
    return promise->get_future();
}

void DefaultThreadPoolExecutor::submit(std::unique_ptr<Task> task)
{
    _queue.enqueue(std::move(task));
}

//! Private members
void DefaultThreadPoolExecutor::init(const size_t numThreads)
{
    try {
        resize(numThreads);
    }
    catch (...) {
        invalidate();
        TRACE("An unhandled exception occurred during initialisation of the DefaultThreadPoolExecutor!");
        throw;
    }
}

void DefaultThreadPoolExecutor::invalidate()
{
    resize(0);

    std::vector<std::unique_ptr<Task>> tasks;
    {
        std::lock_guard lock{_mutex};
        tasks = _queue.close();
    }

    for (const auto& task : tasks) {
        try {
            task->cancel();
            task->run();
        }
        catch (...) { // NOSONAR
            // Ignore
        }
    }

    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard lock{_mutex};
        for (auto& [id, thread] : _threads) {
            threadsToJoin.push_back(std::move(thread));
        }
        _threads.clear();
    }

    for (auto& thread : threadsToJoin) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void DefaultThreadPoolExecutor::worker()
{
    std::unique_ptr<Task> task;

    while (true) {
        if (!_queue.waitDequeue(task)) {
            return;
        }

        try {
            task->run();
        }
        catch (const StopThreadException&) {
            // Gracefully stop the thread execution
            return;
        }
        catch (const OperationCanceledException&) { // NOSONAR
            // Ignore
        }
        catch (const std::exception& ex) {
            TRACE("An unhandled exception occurred during execution of the task. Message: %s", ex.what());
        }
        catch (...) {
            TRACE("An unhandled exception occurred during execution of the task!");
        }
    }
}