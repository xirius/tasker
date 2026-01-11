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

        [[nodiscard]] std::unique_ptr<ChainableTask> clone() const override
        {
            return std::make_unique<RunTaskAdapter>(getExecutor(), nullptr);
        }

        void run() override
        {
            if (_task && !isCanceled() && !getToken().isCancellationRequested()) {
                _task->run();
            }
        }

      private:
        [[nodiscard]] bool isPromised() const noexcept override
        {
            return false;
        }

        void handleException(std::exception_ptr) override
        {
            // Ignore
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

std::unique_ptr<internal::ChainableTask> ScheduledTask::clone() const
{
    auto cloned = std::make_unique<ScheduledTask>(this->getExecutor(), _due, nextSequence());
    cloned->setInterval(_interval);
    cloned->setToken(this->getToken());
    return cloned;
}

void ScheduledTask::run()
{
    if (isPeriodic()) {
        if (this->isCanceled() || this->getToken().isCancellationRequested()) {
            return;
        }

        if (this->hasNext()) {
            if (auto cloned = this->cloneChain(true)) {
                const auto executor = cloned->getExecutor();
                cloned->setToken(this->getToken());
                executor->submit(std::move(cloned));
            }
        }
    }
    else if (const auto next = this->nextTaskAs<RunTaskAdapter*>()) {
        next->run();
    }
    else {
        scheduleNext();
    }
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
        auto adapter = std::make_unique<RunTaskAdapter>(this, std::move(task));
        scheduled = ScheduledTask::create(this, std::chrono::steady_clock::now());
        scheduled->setNext(std::move(adapter));
    }

    { // protected
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
        if (!current->isCanceled() && !current->getToken().isCancellationRequested()) {
            try {
                current->run();
            }
            catch (const std::exception& ex) {
                TRACE("An unhandled exception occurred during execution of the task. Message: %s", ex.what());
            }
            catch (...) {
                TRACE("An unhandled exception occurred during execution of the task!");
            }
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

    if (const auto now = std::chrono::steady_clock::now(); !_queue.empty() && (*_queue.begin())->due() > now) {
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
    if (current->isCanceled() || current->getToken().isCancellationRequested()) {
        return;
    }

    bool reschedule = false;
    if (current->isPeriodic()) {
        // O(1) drift-free catch-up: compute how many intervals we need to jump ahead
        if (const auto interval = current->interval(); interval.count() > 0) {
            const auto prevDue = current->due();
            const auto now = std::chrono::steady_clock::now();

            // We must schedule at least one period ahead of previous due
            // periods = 1 if we are still ahead of now, otherwise jump by the number of missed intervals + 1
            std::int64_t periods = 1;
            if (now > prevDue) {
                const auto behind = now - prevDue; // non-negative
                periods += behind / interval;
            }

            current->setDue(prevDue + interval * periods);
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
