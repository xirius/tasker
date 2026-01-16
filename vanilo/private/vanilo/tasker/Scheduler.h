#ifndef VANILO_SCHEDULER_H
#define VANILO_SCHEDULER_H

#include <vanilo/tasker/Tasker.h>

#include <set>

namespace vanilo::tasker {

    /// ScheduledTask
    /// ============================================================================================

    class ScheduledTask final: public internal::ChainableTask
    {
      public:
        using steady_clock = std::chrono::steady_clock;
        using system_clock = std::chrono::system_clock;
        using Recompute = std::function<steady_clock::time_point()>;

        static std::unique_ptr<ScheduledTask> create(TaskExecutor* executor, steady_clock::time_point due);
        static std::unique_ptr<ScheduledTask> create(
            TaskExecutor* executor, steady_clock::time_point due, std::optional<steady_clock::duration> interval);

        ScheduledTask(TaskExecutor* executor, std::shared_ptr<LifeCycleGuard> guard, steady_clock::time_point due, std::uint64_t sequence);

        [[nodiscard]] std::unique_ptr<ChainableTask> clone() const override;

        void run() override;

        [[nodiscard]] steady_clock::time_point due() const
        {
            return _due;
        }

        void setDue(steady_clock::time_point due);

        [[nodiscard]] std::uint64_t sequence() const
        {
            return _sequence;
        }

        [[nodiscard]] bool isPeriodic() const
        {
            return _interval.has_value();
        }

        [[nodiscard]] steady_clock::duration interval() const
        {
            return _interval.value();
        }

        void setInterval(std::optional<steady_clock::duration> interval);

      private:
        [[nodiscard]] bool isPromised() const noexcept override;
        void handleException([[maybe_unused]] std::exception_ptr exPtr) override;

        steady_clock::time_point _due;
        std::optional<steady_clock::duration> _interval;
        std::uint64_t _sequence; // tie-breaker for strict weak ordering
    };

    /// TaskComparer
    /// ============================================================================================

    struct TaskDueTimeComparator
    {
        bool operator()(const std::unique_ptr<ScheduledTask>& a, const std::unique_ptr<ScheduledTask>& b) const noexcept
        {
            // Order by the earliest due first; tie-break by smaller sequence first
            if (a->due() == b->due())
                return a->sequence() < b->sequence();
            return a->due() < b->due();
        }
    };

    /// Task scheduler
    /// ============================================================================================

    class TaskScheduler final: public TaskExecutor
    {
      public:
        static std::unique_ptr<TaskScheduler> create();

        TaskScheduler();
        ~TaskScheduler() noexcept override;

        [[nodiscard]] size_t count() const override;
        void submit(std::unique_ptr<Task> task) override;

      private:
        void worker();
        bool shouldStop() const noexcept;
        void waitForTasksOrStop(std::unique_lock<std::mutex>& lock);
        bool waitUntilTopIsDue(std::unique_lock<std::mutex>& lock);
        std::unique_ptr<ScheduledTask> popTopLocked();
        void rescheduleIfNeeded(std::unique_ptr<ScheduledTask>& current);

        std::atomic_bool _stop{false};
        mutable std::mutex _mutex;
        std::condition_variable _condition;
        std::multiset<std::unique_ptr<ScheduledTask>, TaskDueTimeComparator> _queue;
        std::thread _worker;
    };

} // namespace vanilo::tasker

#endif // VANILO_SCHEDULER_H
