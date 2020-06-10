#ifndef INC_CF1B33A15FDE47BDA967EACB24A90BED
#define INC_CF1B33A15FDE47BDA967EACB24A90BED

#include <vanilo/Export.h>
#include <vanilo/tasker/CancellationToken.h>

#include <cassert>
#include <future>
#include <iostream>
#include <memory>
#include <optional>

namespace vanilo::tasker {

    class TaskExecutor;

    /// Task interface
    /// ========================================================================

    class Task
    {
      public:
        virtual ~Task() = default;

        virtual void run() = 0;
    };

    /// TaskExecutor interface
    /// ========================================================================

    class TaskExecutor
    {
      public:
        virtual ~TaskExecutor() = default;

        virtual size_t count() const                    = 0;
        virtual void submit(std::unique_ptr<Task> task) = 0;
    };

    /// QueuedTaskExecutor interface
    /// ========================================================================

    class QueuedTaskExecutor: public TaskExecutor
    {
      public:
        static std::unique_ptr<QueuedTaskExecutor> create();

        /**
         * @param maxCount the maximum number of task to process.
         * @return the number of tasks that still need to be processed.
         */
        virtual size_t process(size_t maxCount) = 0;
    };

    /**
     * This exception occur when you're waiting for a result, then a cancellation is notified.
     */
    class CancellationException final: public std::exception
    {
      public:
        const char* what() const noexcept override
        {
            return "A task was canceled.";
        }
    };

    namespace details {

        /**
         * Holds cancellation token and exception callback of the current task.
         */
        class TaskContext
        {
          public:
            TaskContext() = default;

            explicit TaskContext(CancellationToken token): _token{std::move(token)}
            {
            }

            bool isCanceled() const noexcept
            {
                return _token.isCanceled();
            }

            bool hasExceptionCallback() const noexcept
            {
                return (bool)_onException;
            }

            void onException(const std::exception& ex) const
            {
                if (_onException) {
                    _onException(ex);
                }
                else {
                    std::cerr << "An unhandled exception occurred during task execution. Message: " << ex.what() << std::endl;
                }
            }

            void setExceptionCallback(std::function<void(const std::exception&)> callback)
            {
                _onException = std::move(callback);
            }

          private:
            std::function<void(const std::exception&)> _onException;
            CancellationToken _token;
        };

        /**
         * Base class of a chainable task.
         */
        class LinkedTask: public Task
        {
          public:
            explicit LinkedTask(TaskExecutor* executor): _executor{executor}
            {
            }

            void setContext(TaskContext context)
            {
                _context = std::move(context);
            }

            void setNext(std::unique_ptr<LinkedTask> task)
            {
                assert(task && "Task cannot be null!");
                _next = std::move(task);
            }

          protected:
            template <typename Callable, typename Result, typename Arg>
            friend class BaseTask;

            virtual void handleException() = 0;

            LinkedTask* lastTask()
            {
                auto task = this;

                while (task->_next) {
                    task = task->_next.get();
                }

                return task;
            }

            inline void scheduleNext()
            {
                _next->setContext(std::move(_context));
                _next->_executor->submit(std::move(_next));
            }

            TaskContext _context;
            TaskExecutor* _executor;
            std::unique_ptr<LinkedTask> _next;
        };

        /**
         * Argument dependent generic chainable task.
         * @tparam Arg
         */
        template <typename Arg>
        class ParametricTask: public LinkedTask
        {
          public:
            using LinkedTask::LinkedTask;

            void setArgument(Arg&& arg)
            {
                _param = std::move(arg);
            }

          protected:
            Arg _param;
        };

        /**
         * Argument dependent generic chainable task specialization in case the argument is void.
         */
        template <>
        class ParametricTask<void>: public LinkedTask
        {
          public:
            using LinkedTask::LinkedTask;
        };

        /**
         * Generic task returning Result and taking Arg as a parameter.
         * @tparam Callable The type of the task
         * @tparam Result The result type returned by the task
         * @tparam Arg The argument type of the task
         */
        template <typename Callable, typename Result, typename Arg>
        class BaseTask: public ParametricTask<Arg>
        {
          public:
            explicit BaseTask(TaskExecutor* executor, Callable&& task): ParametricTask<Arg>{executor}, _task{std::move(task)}
            {
            }

            std::future<Result> getFuture()
            {
                _promise.emplace();
                return _promise->get_future();
            }

          protected:
            inline void dispatchException()
            {
                LinkedTask* task = this->lastTask();
                task->setContext(std::move(this->_context));
                task->handleException();
            }

            inline bool hasPromise() const noexcept
            {
                return _promise.has_value();
            }

            template <typename T>
            inline void setPromiseValue(T&& value)
            {
                _promise->set_value(std::forward<T>(value));
            }

            inline Callable& task()
            {
                return _task;
            }

          private:
            void handleException() override
            {
                try {
                    if (this->_context.hasExceptionCallback()) {
                        onFailure();
                    }
                    else if (_promise) { // If std::future was taken.
                        _promise->set_exception(std::current_exception());
                    }
                }
                catch (...) {
                    // set_exception() or onFailure may throw too. Ignore it.
                }
            }

            inline void onFailure() const
            {
                try {
                    std::rethrow_exception(std::current_exception());
                }
                catch (const std::exception& ex) {
                    this->_context.onException(ex);
                }
                catch (...) {
                    this->_context.onException(std::runtime_error{"Unknown error occurred during execution of the task."});
                }
            }

            std::optional<std::promise<Result>> _promise;
            Callable _task;
        };

        /**
         * Generic task specialization.
         * @tparam Callable The type of the task
         * @tparam Result The result type returned by the task
         * @tparam Arg The argument type of the task
         */
        template <typename Callable, typename Result, typename Arg>
        class Invocable: public BaseTask<Callable, Result, Arg>
        {
          public:
            using BaseTask<Callable, Result, Arg>::BaseTask;

            void run() override
            {
                try {
                    if (this->_next) {
                        // Case 1 : we have a next task to execute
                        auto next = dynamic_cast<ParametricTask<Result>*>(this->_next.get());
                        next->setArgument(executeTask());
                        this->scheduleNext();
                    }
                    else if (this->hasPromise()) {
                        // Case 2 : Future was taken
                        this->setPromiseValue(executeTask());
                    }
                    else {
                        // Case 3 : No result and no next task
                        executeTask();
                    }
                }
                catch (...) {
                    this->dispatchException();
                }
            }

          private:
            inline Result executeTask()
            {
                return this->task()(std::move(this->_param));
            }
        };

    } // namespace details

} // namespace vanilo::tasker

#endif // INC_CF1B33A15FDE47BDA967EACB24A90BED