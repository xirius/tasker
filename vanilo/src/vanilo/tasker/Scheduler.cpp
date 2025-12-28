#include "vanilo/tasker/Scheduler.h"

#include <memory>

using namespace vanilo::tasker;

namespace {
    /// Generates a monotonically increasing sequence number for strict-weak ordering tie-breaks
    std::uint64_t nextSequence()
    {
        static std::atomic_uint64_t counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    /// Adapter to allow scheduling of arbitrary Task instances that are not chainable/scheduled.
    class RunTaskAdapter final: public internal::ChainableTask
    {
      public:
        RunTaskAdapter(TaskExecutor* executor, std::unique_ptr<Task> task): ChainableTask{executor}, _task{std::move(task)}
        {
        }

        void run() override
        {
            if (_task && !isCanceled()) {
                _task->run();
            }

            scheduleNext();
        }

      private:
        [[nodiscard]] bool isPromised() const noexcept override
        {
            return false;
        }

        void handleException(std::exception_ptr) override
        {
        }

        std::unique_ptr<Task> _task;
    };
} // namespace

/// ScheduledTask
/// ============================================================================================

std::unique_ptr<ScheduledTask> ScheduledTask::create(TaskExecutor* executor, steady_clock::time_point due)
{
    return std::make_unique<ScheduledTask>(executor, due, nextSequence());
}

std::unique_ptr<ScheduledTask> ScheduledTask::create(
    TaskExecutor* executor, steady_clock::time_point due, const std::optional<steady_clock::duration> interval)
{
    auto task = std::make_unique<ScheduledTask>(executor, due, nextSequence());
    task->setInterval(interval);
    return task;
}

ScheduledTask::ScheduledTask(TaskExecutor* executor, const steady_clock::time_point due, const std::uint64_t sequence)
    : ChainableTask{executor}, _due{due}, _sequence{sequence}
{
}

void ScheduledTask::run()
{
    scheduleNext();
}

void ScheduledTask::setDue(const steady_clock::time_point due)
{
    _due = due;
}

void ScheduledTask::setInterval(const std::optional<steady_clock::duration> interval)
{
    _interval = interval;
}

[[nodiscard]] bool ScheduledTask::isPromised() const noexcept
{
    return false;
}

void ScheduledTask::handleException(std::exception_ptr exPtr)
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

TaskScheduler::~TaskScheduler() noexcept
{
    _stop.store(true, std::memory_order_relaxed);
    _condition.notify_one();
    if (_worker.joinable()) {
        _worker.join();
    }
}

size_t TaskScheduler::count() const
{
    std::scoped_lock lock(_mutex);
    return _queue.size();
}

void TaskScheduler::submit(std::unique_ptr<Task> task)
{
    std::unique_ptr<ScheduledTask> scheduled;

    if (const auto scheduledTask = dynamic_cast<ScheduledTask*>(task.get())) {
        [[maybe_unused]] auto unused = task.release();
        scheduled.reset(scheduledTask);
    }
    else if (const auto chainableTask = dynamic_cast<internal::ChainableTask*>(task.get())) {
        [[maybe_unused]] auto unused = task.release();
        scheduled = ScheduledTask::create(this, std::chrono::steady_clock::now());
        std::unique_ptr<internal::ChainableTask> chainable;
        chainable.reset(chainableTask);
        scheduled->setNext(std::move(chainable));
    }
    else {
        // Wrap a generic Task into a chainable adapter so it can be scheduled and executed
        auto adapter = std::make_unique<RunTaskAdapter>(task->, std::move(task));
        scheduled = ScheduledTask::create(this, std::chrono::steady_clock::now());
        scheduled->setNext(std::move(adapter));
    }

    {
        std::scoped_lock lock(_mutex);
        _queue.insert(std::move(scheduled));
    }
    _condition.notify_one();
}

void TaskScheduler::worker()
{
    while (true) {
        std::unique_ptr<ScheduledTask> current;
        {
            std::unique_lock lock{_mutex};
            if (shouldStop()) {
                break;
            }

            waitForTasksOrStop(lock);
            if (shouldStop()) {
                break;
            }

            if (waitUntilTopIsDue(lock)) {
                continue; // re-evaluate heap after wake-up
            }

            current = popTopLocked();
        }

        // Execute outside the lock
        if (!current->isCanceled()) {
            current->run();
        }

        // Reschedule if periodic
        rescheduleIfNeeded(current);
    }
}

bool TaskScheduler::shouldStop() const noexcept
{
    return _stop.load(std::memory_order_relaxed);
}

void TaskScheduler::waitForTasksOrStop(std::unique_lock<std::mutex>& lock)
{
    if (_queue.empty()) {
        _condition.wait(lock, [this] { return shouldStop() || !_queue.empty(); });
    }
}

bool TaskScheduler::waitUntilTopIsDue(std::unique_lock<std::mutex>& lock)
{
    if (_queue.empty())
        return false;

    if (const auto now = std::chrono::steady_clock::now(); !(_queue.empty()) && (*_queue.begin())->due() > now) {
        auto until = (*_queue.begin())->due();
        _condition.wait_until(lock, until, [this, until] { return shouldStop() || _queue.empty() || (*_queue.begin())->due() < until; });
        return true;
    }
    return false;
}

std::unique_ptr<ScheduledTask> TaskScheduler::popTopLocked()
{
    // pre: lock is held, queue is not empty
    auto nodeHandle = _queue.extract(_queue.begin());
    return std::move(nodeHandle.value());
}

void TaskScheduler::rescheduleIfNeeded(std::unique_ptr<ScheduledTask>& current)
{
    if (current->isCanceled()) {
        return;
    }

    bool reschedule = false;
    if (current->isPeriodic()) {
        // drift-free: step forward from prior 'due'
        current->setDue(current->due() + current->interval());

        // catch up if we fell behind
        const auto now = std::chrono::steady_clock::now();

        while (current->due() <= now) {
            current->setDue(current->due() + current->interval());
        }

        reschedule = true;
    }

    if (reschedule) {
        {
            std::scoped_lock lock(_mutex);
            _queue.insert(std::move(current));
        }

        _condition.notify_one();
    }
}
