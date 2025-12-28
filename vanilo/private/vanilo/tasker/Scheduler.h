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
            TaskExecutor* executor, steady_clock::time_point due, std::optional<steady_clock::duration> period);

        ScheduledTask(TaskExecutor* executor, steady_clock::time_point due, std::uint64_t sequence);

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
            return _period.has_value();
        }

        [[nodiscard]] steady_clock::duration period() const
        {
            return _period.value();
        }

        void setPeriod(std::optional<steady_clock::duration> period);

      private:
        [[nodiscard]] bool isPromised() const noexcept override;
        void handleException([[maybe_unused]] std::exception_ptr exPtr) override;

        steady_clock::time_point _due;
        std::optional<steady_clock::duration> _period;
        std::uint64_t _sequence; // tie-breaker for strict weak ordering
    };

    /// TaskComparer
    /// ============================================================================================

    struct TaskComparer
    {
        bool operator()(const std::unique_ptr<ScheduledTask>& a, const std::unique_ptr<ScheduledTask>& b) const noexcept
        {
            if (a->due() == b->due())
                return a->sequence() > b->sequence();
            return a->due() > b->due(); // min-heap via greater-than
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

        // Extracted helpers to reduce method length/complexity (SonarQube)
        bool shouldStop() const noexcept;
        void waitForTasksOrStop(std::unique_lock<std::mutex>& lock);
        bool waitUntilTopIsDue(std::unique_lock<std::mutex>& lock);
        std::unique_ptr<ScheduledTask> popTopLocked();
        void rescheduleIfNeeded(std::unique_ptr<ScheduledTask>& current);

        std::atomic_bool _stop{false};
        mutable std::mutex _mutex;
        std::condition_variable _condition;
        std::multiset<std::unique_ptr<ScheduledTask>, TaskComparer> _queue;
        std::thread _worker;
    };

} // namespace vanilo::tasker

#endif // VANILO_SCHEDULER_H
