#ifndef VANILO_TASKER_TASK_EXECUTOR_H
#define VANILO_TASKER_TASK_EXECUTOR_H

#include <vanilo/Export.h>

#include <cstddef>
#include <future>
#include <limits>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace vanilo::tasker {

    class Task;

    namespace detail {
        class ExecutorCore;
        struct ExecutorAccess;
    } // namespace detail

    /// TaskExecutor
    /// ============================================================================================

    /**
     * Runs tasks: the base of ManualExecutor and ThreadPoolExecutor.
     *
     * Destroying an executor cancels the tasks that are still queued, and tasks submitted to it later (for
     * example the continuations of chains running elsewhere) are canceled instead of queued: their onError
     * handlers and TaskHandles receive an OperationCanceledException. The executor itself must outlive the
     * calls made on it.
     */
    class VANILO_EXPORT TaskExecutor
    {
      public:
        virtual ~TaskExecutor();

        TaskExecutor(const TaskExecutor& other) = delete;
        TaskExecutor& operator=(const TaskExecutor& other) = delete;

        /**
         * @return The number of tasks waiting in the queue.
         */
        [[nodiscard]] std::size_t pendingCount() const noexcept;

        /**
         * Queues a task object. If the executor is shutting down, the task's cancel() is called instead and
         * it never runs.
         */
        void submit(std::unique_ptr<Task> task);

      protected:
        explicit TaskExecutor(std::shared_ptr<detail::ExecutorCore> core) noexcept;

      private:
        friend struct detail::ExecutorAccess;

        std::shared_ptr<detail::ExecutorCore> _core;
    };

    /// ManualExecutor
    /// ============================================================================================

    /**
     * An executor that runs its tasks on the thread that calls runPending() or runUntil(), for example an event
     * loop: other threads hand work to that thread by queueing tasks on it.
     */
    class VANILO_EXPORT ManualExecutor final: public TaskExecutor
    {
        struct Private;

      public:
        static std::unique_ptr<ManualExecutor> create();

        explicit ManualExecutor(Private);
        ~ManualExecutor() override;

        /**
         * Runs the queued tasks until the queue is empty or maxCount tasks ran, including tasks queued by
         * the tasks it runs.
         * @param maxCount The maximum number of tasks to run.
         * @return The number of tasks still queued.
         */
        std::size_t runPending(std::size_t maxCount = std::numeric_limits<std::size_t>::max());

        /**
         * Runs tasks, waiting for new ones, until a stop is requested through the token. Tasks that are already
         * queued when it is requested still run before it returns.
         * @param token Ends the loop.
         * @return The number of tasks still queued.
         */
        std::size_t runUntil(std::stop_token token);
    };

    /// ThreadPoolExecutor
    /// ============================================================================================

    /**
     * An executor that runs its tasks on a pool of threads. Each thread has its own queue: tasks queued from a
     * pool thread go to its queue, others are spread over the queues, and idle threads take work from the
     * queues of busy ones.
     */
    class VANILO_EXPORT ThreadPoolExecutor final: public TaskExecutor
    {
        struct Private;

      public:
        /**
         * @return The number of hardware threads (at least 1): the default size of a pool.
         */
        [[nodiscard]] static std::size_t defaultThreadCount() noexcept;

        /**
         * Creates a pool with defaultThreadCount() threads.
         */
        static std::unique_ptr<ThreadPoolExecutor> create();

        /**
         * Creates a pool with the given number of threads (it may be 0, see resize()).
         */
        static std::unique_ptr<ThreadPoolExecutor> create(std::size_t threadCount);

        ThreadPoolExecutor(Private, std::size_t threadCount);
        ~ThreadPoolExecutor() override;

        /**
         * Changes the number of threads. A thread that is removed finishes its current task first; its queued
         * tasks are taken over by the other threads.
         * @return A future that is ready once the removed threads have stopped.
         */
        std::future<void> resize(std::size_t threadCount);

        /**
         * @return True if the thread belongs to the pool.
         */
        [[nodiscard]] bool containsThread(std::thread::id threadId) const;

        /**
         * @return The number of threads in the pool.
         */
        [[nodiscard]] std::size_t threadCount() const noexcept;

        /**
         * @return The ids of the threads in the pool.
         */
        [[nodiscard]] std::vector<std::thread::id> threadIds() const;
    };

    namespace detail {
        struct ExecutorAccess
        {
            static const std::shared_ptr<ExecutorCore>& core(const TaskExecutor& executor) noexcept
            {
                return executor._core;
            }
        };

        /**
         * The core of an executor (null for no executor): what tasks hold to reach it.
         */
        inline std::shared_ptr<ExecutorCore> coreOf(const TaskExecutor* executor) noexcept
        {
            return executor ? ExecutorAccess::core(*executor) : nullptr;
        }
    } // namespace detail

} // namespace vanilo::tasker

#endif // VANILO_TASKER_TASK_EXECUTOR_H
