#ifndef VANILO_TASKER_EXECUTOR_CORE_H
#define VANILO_TASKER_EXECUTOR_CORE_H

#include <vanilo/tasker/detail/Job.h>

#include <cstddef>

namespace vanilo::tasker::detail {

    /**
     * The shared part of an executor: its queue. Tasks waiting in other executors' queues hold it, so they can
     * still be handed to it after the executor object is gone: it then no longer takes jobs.
     */
    class ExecutorCore
    {
      public:
        ExecutorCore() = default;
        virtual ~ExecutorCore() = default;

        ExecutorCore(const ExecutorCore&) = delete;
        ExecutorCore& operator=(const ExecutorCore&) = delete;

        /**
         * Queues the job.
         * @return True if queued; false (the job left untouched) if the executor no longer takes jobs.
         */
        virtual bool tryEnqueue(Job& job) = 0;

        /**
         * @return The number of queued jobs.
         */
        [[nodiscard]] virtual std::size_t pendingCount() const noexcept = 0;
    };

    /**
     * Runs a job, reporting an exception that escapes it to the unhandled-exception handler.
     */
    void runJob(Job& job) noexcept;

} // namespace vanilo::tasker::detail

#endif // VANILO_TASKER_EXECUTOR_CORE_H
