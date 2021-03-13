#include <vanilo/tasker/DefaultThreadPoolExecutor.h>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

/// ThreadPool system tasks
/// ============================================================================================

class StopThreadException final: public std::exception
{
  public:
    [[nodiscard]] const char* what() const noexcept override
    {
        return "Stop the thread execution exception!";
    }
};

class StopThreadTask: public Task
{
  public:
    explicit StopThreadTask(TaskExecutor* executor, std::thread thread): _executor{executor}, _thread{std::move(thread)}
    {
    }

    void cancel() noexcept override
    {
    }

    void run() override
    {
        // Detach theIf the task is running the thread that is supposed to be exited
        if (std::this_thread::get_id() == _thread.get_id()) {
            _thread.detach();
            throw StopThreadException{};
        }

        _executor->submit(std::make_unique<StopThreadTask>(_executor, std::move(_thread)));
    }

  private:
    TaskExecutor* _executor;
    std::thread _thread;
};

/// DefaultThreadPoolExecutor implementation
/// ============================================================================================

DefaultThreadPoolExecutor::DefaultThreadPoolExecutor(size_t numThreads)
{
    init(numThreads);
}

DefaultThreadPoolExecutor::~DefaultThreadPoolExecutor()
{
    invalidate();
}

size_t DefaultThreadPoolExecutor::count() const
{
    return _queue.size();
}

void DefaultThreadPoolExecutor::resize(size_t numThreads)
{
    std::lock_guard<std::mutex> _lock{_mutex};

    if (_threads.size() < numThreads) {
        for (auto i = _threads.size(); i < numThreads; i++) {
            _threads.enqueue(std::thread{&DefaultThreadPoolExecutor::worker, this});
        }
    }
    else {
        std::thread thread;

        for (auto i = _threads.size(); i >= numThreads; i++) {
            if (_threads.tryDequeue(thread)) {
                submit(std::make_unique<StopThreadTask>(this, std::move(thread)));
            }
        }
    }
}

void DefaultThreadPoolExecutor::submit(std::unique_ptr<Task> task)
{
    _queue.enqueue(std::move(task));
}

//! Private members
void DefaultThreadPoolExecutor::init(size_t numThreads)
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
    auto tasks = _queue.invalidate();

    for (auto& task : tasks) {
        task->cancel();
        // Not enough !!!
    }

    std::thread thread;
    while (_threads.tryDequeue(thread)) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void DefaultThreadPoolExecutor::worker()
{
    std::unique_ptr<Task> task;

    try {
        while (true) {
            if (!_queue.waitDequeue(task)) {
                return;
            }

            try {
                task->run();
            }
            catch (const StopThreadException&) {
                throw; // This thread was asked to exit so propagate the exception
            }
            catch (const std::exception& ex) {
                TRACE("An unhandled exception occurred during execution of the task. Message: %s", ex.what());
            }
            catch (...) {
                TRACE("An unhandled exception occurred during execution of the task!");
            }
        }
    }
    catch (const StopThreadException&) {
        return; // Gracefully stop the thread execution
    }
}
