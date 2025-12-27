#ifndef VANILO_SCHEDULER_H
#define VANILO_SCHEDULER_H

#include <vanilo/tasker/Tasker.h>

#include <queue>

namespace vanilo::tasker {

    /// DelayedTask
    /// ============================================================================================

    class DelayedTask final: public internal::ChainableTask
    {
      public:
        static std::unique_ptr<DelayedTask> create(TaskExecutor* executor, const std::chrono::steady_clock::time_point due)
        {
            return std::make_unique<DelayedTask>(executor, due);
        }

        DelayedTask(TaskExecutor* executor, std::chrono::steady_clock::time_point due);

        void run() override;

        [[nodiscard]] std::chrono::steady_clock::time_point due() const
        {
            return _due;
        }

        [[nodiscard]] std::uint64_t sequence() const
        {
            return _sequence;
        }

      private:
        [[nodiscard]] bool isPromised() const noexcept override;
        void handleException(std::exception_ptr exPtr) override;

        std::chrono::steady_clock::time_point _due;
        std::uint64_t _sequence; // tie-breaker for strict weak ordering
    };

    /// TaskComparer
    /// ============================================================================================

    struct TaskComparer
    {
        bool operator()(const std::unique_ptr<DelayedTask>& a, const std::unique_ptr<DelayedTask>& b) const noexcept
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
        ~TaskScheduler() override;

        [[nodiscard]] size_t count() const override;
        void submit(std::unique_ptr<Task> task) override;

      private:
        void worker();

        std::atomic_bool _stop{false};
        std::mutex _mutex;
        std::condition_variable _condition;
        std::priority_queue<std::unique_ptr<Task>, std::vector<std::unique_ptr<Task>>, TaskComparer> _queue;
        std::thread _worker;
    };

} // namespace vanilo::tasker

#endif // VANILO_SCHEDULER_H
