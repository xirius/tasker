#ifndef INC_CF1B33A15FDE47BDA967EACB24A90BED
#define INC_CF1B33A15FDE47BDA967EACB24A90BED

#include <vanilo/Export.h>
#include <vanilo/core/Binder.h>
#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/CancellationToken.h>

#include <any>
#include <cassert>
#include <future>
#include <iostream>

namespace vanilo::tasker {

    /// Cancellation exception
    /// ============================================================================================

    /**
     * This exception occur when you're waiting for a result, then a cancellation is notified.
     */
    class CancellationException final: public std::exception
    {
      public:
        [[nodiscard]] const char* what() const noexcept override
        {
            return "A task was canceled.";
        }
    };

    /**
     * Object that manages the execution of the scheduled tasks.
     */
    class TaskExecutor;

    /// Task interface
    /// ============================================================================================

    class VANILO_EXPORT Task
    {
      protected:
        template <typename Signature>
        class Builder;

      public:
        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<TaskFunc>>
        static TaskBuilder run(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

        virtual ~Task() = default;

        virtual void run() = 0;
    };

    /// TaskExecutor interface
    /// ============================================================================================

    class TaskExecutor
    {
      public:
        virtual ~TaskExecutor() = default;

        [[nodiscard]] virtual size_t count() const      = 0;
        virtual void submit(std::unique_ptr<Task> task) = 0;
    };

    /// QueuedTaskExecutor interface
    /// ============================================================================================

    class VANILO_EXPORT QueuedTaskExecutor: public TaskExecutor
    {
      public:
        static std::unique_ptr<QueuedTaskExecutor> create();

        /**
         * @param maxCount the maximum number of task to process.
         * @return the number of tasks that still need to be processed.
         */
        virtual size_t process(size_t maxCount) = 0;
    };

    /// ThreadPoolExecutor interface
    /// ============================================================================================

    class VANILO_EXPORT ThreadPoolExecutor: public TaskExecutor
    {
      public:
        static std::unique_ptr<ThreadPoolExecutor> create();

        /**
         * @param maxCount the maximum number of task to process.
         * @return the number of tasks that still need to be processed.
         */
        virtual size_t process(size_t maxCount) = 0;
    };

    namespace internal {

        class ChainableTask;

        /// Base task builder
        /// ========================================================================================

        class BaseTaskBuilder
        {
            friend Task;

          public:
            // Disabled copy assignment and copy construction to force only one operation on the object.
            BaseTaskBuilder(const BaseTaskBuilder& other) = delete;

            // Disabled copy assignment and copy construction to force only one operation on the object.
            BaseTaskBuilder& operator=(const BaseTaskBuilder& other) = delete;

            virtual ~BaseTaskBuilder();

          protected:
            explicit BaseTaskBuilder(std::unique_ptr<internal::ChainableTask> task, internal::ChainableTask* last);

            std::unique_ptr<internal::ChainableTask> _task;
            internal::ChainableTask* _last;
        };
    } // namespace internal

    /// Task::Builder declaration
    /// ============================================================================================

    template <typename Signature>
    class Task::Builder: public internal::BaseTaskBuilder
    {
        friend Task;

      public:
        using ResultType = typename core::traits::FunctionTraits<Signature>::ReturnType;
        using FirstArg   = typename core::traits::FunctionTraits<Signature>::template Arg<0>;
        using PureArgs   = typename core::traits::FunctionTraits<Signature>::PureArgsType;

        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<TaskFunc>>
        TaskBuilder then(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

      private:
        using BaseTaskBuilder::BaseTaskBuilder;
    };

    namespace internal {

        /**
         * Generic task specialization.
         * @tparam Callable The type of the task
         * @tparam Result The result type returned by the task
         * @tparam Arg The type of the argument  which is taken by the task
         */
        template <typename Callable, typename Result, typename Arg>
        class Invocable;

        /**
         * Represents an abstract generic chainable task.
         */
        class ChainableTask: public Task
        {
          public:
            explicit ChainableTask(TaskExecutor* executor): _executor{executor}
            {
            }

            [[nodiscard]] inline TaskExecutor* getExecutor() const
            {
                return _executor;
            }

            [[nodiscard]] inline CancellationToken getToken() const
            {
                return _token;
            }

            void setToken(CancellationToken token)
            {
                _token = std::move(token);
            }

            void setNext(std::unique_ptr<ChainableTask> task)
            {
                assert(task && "Task cannot be null!");
                _next = std::move(task);
            }

          protected:
            ChainableTask* lastTask()
            {
                auto task = this;

                while (task->_next) {
                    task = task->_next.get();
                }

                return task;
            }

            inline void scheduleNext()
            {
                _next->setToken(std::move(_token));
                _next->_executor->submit(std::move(_next));
            }

            TaskExecutor* _executor;
            CancellationToken _token;
            std::unique_ptr<ChainableTask> _next;
        };

        /**
         * Represents an abstract argument dependent generic chainable task.
         */
        template <typename Arg>
        class ParameterizedChainableTask: public ChainableTask
        {
          public:
            using ChainableTask::ChainableTask;

            void setArgument(Arg&& arg)
            {
                _param = std::move(arg);
            }

          protected:
            Arg _param;
        };

        /**
         * Represents a specialization of an abstract generic chainable task without parameter.
         */
        template <>
        class ParameterizedChainableTask<void>: public ChainableTask
        {
          public:
            using ChainableTask::ChainableTask;
        };

        template <typename Callable, typename Result, typename Arg>
        class BaseTask: public ParameterizedChainableTask<Arg>
        {
            using ErrorHandler = bool (*)(TaskExecutor* executor, Callable&, std::any&, const std::exception_ptr&);

          public:
            explicit BaseTask(TaskExecutor* executor, Callable&& task)
                : ParameterizedChainableTask<Arg>{executor}, _task{std::move(task)}, _errorExecutor{nullptr},
                  _errorHandler{[](TaskExecutor* executor, Callable&, std::any&, const std::exception_ptr&) { return false; }}
            {
            }

            template <typename Functor, typename... Args>
            void setupExceptionCallback(TaskExecutor* executor, Functor&& functor, Args... args)
            {
                _errorExecutor = executor;
                _errorMetadata = std::make_any<std::tuple<Functor, std::tuple<Args...>>>(
                    std::make_tuple(std::forward<Functor>(functor), std::make_tuple(std::forward<Args>(args)...)));

                _errorHandler = [](TaskExecutor* executor, Callable& task, std::any& metadata, const std::exception_ptr& exPtr) {
                    auto exceptionTask = core::binder::bind(
                        [](Callable& task, std::any& metadata, std::exception_ptr& exPtr) {
                            auto [func, args] = std::move(std::any_cast<std::tuple<Functor, std::tuple<Args...>>>(metadata));

                            try {
                                try {
                                    std::rethrow_exception(exPtr);
                                }
                                catch (std::exception& ex) {
                                    task.rebindPrepend(
                                        std::forward<Functor>(func), std::forward<Args>(std::get<Args>(args))..., std::ref(ex))();
                                }
                            }
                            catch (...) {
                                /// Ignore or log the error coming form the exception handler callback
                                std::cout << "WTF !!!!!" << std::endl;
                                TRACE("WTF !!!!!");
                            }
                        },
                        std::move(task), std::move(metadata), exPtr);

                    // If there is an another executor to schedule the task on
                    auto invocable =
                        std::make_unique<internal::Invocable<decltype(exceptionTask), void, void>>(executor, std::move(exceptionTask));
                    invocable->run();
                    // else
                    // exceptionTask();

                    return true; // Exception was handled
                };
            }

          protected:
            void handleException(const std::exception_ptr& exPtr)
            {
                _errorHandler(_errorExecutor, _task, _errorMetadata, exPtr);
            }

            template <typename T = Arg, typename std::enable_if_t<std::is_void_v<T>, bool> = true>
            inline Result executeTask()
            {
                return _task();
            }

            template <typename T = Arg, typename std::enable_if_t<!std::is_void_v<T>, bool> = true>
            inline Result executeTask()
            {
                return invoke(this->_param);
            }

            template <typename Param, typename = std::enable_if_t<!core::IsTuple<Param>::value>>
            inline Result invoke(Param& param)
            {
                return _task(param);
            }

            template <typename... Args, typename Packed = std::tuple<Args...>, typename = std::enable_if_t<core::IsTuple<Packed>::value>>
            inline Result invoke(std::tuple<Args...>& args)
            {
                return core::InvokeHelper<Result>::invoke(_task, args, std::make_index_sequence<sizeof...(Args)>{});
            }

          private:
            Callable _task;
            TaskExecutor* _errorExecutor{};
            std::any _errorMetadata;
            ErrorHandler _errorHandler;
        };

        // class PromisedInvocable

        template <typename Callable, typename Result, typename Arg>
        class Invocable: public BaseTask<Callable, Result, Arg>
        {
          public:
            explicit Invocable(TaskExecutor* executor, Callable&& task): BaseTask<Callable, Result, Arg>(executor, std::move(task))
            {
                std::cout << "class Invocable: public BaseTask<Callable, Result, Arg>" << std::endl; //=====================================
            }

            void run() override
            {
                try {
                    if (this->_next) {
                        auto next = dynamic_cast<ParameterizedChainableTask<Result>*>(this->_next.get());
                        next->setArgument(this->executeTask());
                        this->scheduleNext();
                    }
                    else {
                        this->executeTask();
                    }
                }
                catch (...) {
                    this->handleException(std::current_exception());
                }
            }
        };

        template <typename Callable, typename Result>
        class Invocable<Callable, Result, void>: public BaseTask<Callable, Result, void>
        {
          public:
            explicit Invocable(TaskExecutor* executor, Callable&& task): BaseTask<Callable, Result, void>(executor, std::move(task))
            {
                std::cout << "class Invocable<Callable, Result, void>: public BaseTask<Callable, Result, void>" << std::endl; //============
            }

            void run() override
            {
                try {
                    if (this->_next) {
                        auto next = dynamic_cast<ParameterizedChainableTask<Result>*>(this->_next.get());
                        next->setArgument(this->executeTask());
                        this->scheduleNext();
                    }
                    else {
                        this->executeTask();
                    }
                }
                catch (...) {
                    this->handleException(std::current_exception());
                }
            }
        };

        template <typename Callable, typename Arg>
        class Invocable<Callable, void, Arg>: public BaseTask<Callable, void, Arg>
        {
          public:
            explicit Invocable(TaskExecutor* executor, Callable&& task): BaseTask<Callable, void, Arg>(executor, std::move(task))
            {
                std::cout << "class Invocable<Callable, void, Arg>: public BaseTask<Callable, void, Arg>" << std::endl; //==================
            }

            void run() override
            {
                try {
                    this->executeTask();

                    if (this->_next) {
                        this->scheduleNext();
                    }
                }
                catch (...) {
                    this->handleException(std::current_exception());
                }
            }
        };

        template <typename Callable>
        class Invocable<Callable, void, void>: public BaseTask<Callable, void, void>
        {
          public:
            explicit Invocable(TaskExecutor* executor, Callable&& task): BaseTask<Callable, void, void>(executor, std::move(task))
            {
                std::cout << "class Invocable<Callable, void, void>: public BaseTask<Callable, void, void>" << std::endl; //================
            }

            void run() override
            {
                try {
                    this->executeTask();

                    if (this->_next) {
                        this->scheduleNext();
                    }
                }
                catch (...) {
                    this->handleException(std::current_exception());
                }
            }
        };

        /// Base task builder implementation
        /// ========================================================================================

        inline BaseTaskBuilder::BaseTaskBuilder(std::unique_ptr<internal::ChainableTask> task, internal::ChainableTask* last)
            : _task{std::move(task)}, _last{last}
        {
        }

        inline BaseTaskBuilder::~BaseTaskBuilder()
        {
            std::cout << "BaseTaskBuilder::~BaseTaskBuilder()" << std::endl; ///////////////////////////////////////////////////////////////

            if (_task) {
                std::cout << "BaseTaskBuilder::~BaseTaskBuilder() => if (_task)" << std::endl; /////////////////////////////////////////////
                _task->getExecutor()->submit(std::move(_task));
            }
        }

        /// BuilderHelper implementation
        /// ========================================================================================

        template <typename Result, typename Arg, bool HasToken>
        struct BuilderHelper;

        template <typename Result, typename Arg>
        struct BuilderHelper<Result, Arg, true>
        {
            template <typename TaskFunc, typename... Args>
            inline static auto createInvocable(TaskExecutor* executor, CancellationToken token, TaskFunc&& func, Args&&... args)
            {
                auto task = core::binder::bind(std::forward<TaskFunc>(func), token, std::forward<Args>(args)...);
                return std::make_unique<internal::Invocable<decltype(task), Result, Arg>>(executor, std::move(task));
            }
        };

        template <typename Result, typename Arg>
        struct BuilderHelper<Result, Arg, false>
        {
            template <typename TaskFunc, typename... Args>
            inline static auto createInvocable(TaskExecutor* executor, const CancellationToken&, TaskFunc&& func, Args&&... args)
            {
                auto task = core::binder::bind(std::forward<TaskFunc>(func), std::forward<Args>(args)...);
                return std::make_unique<internal::Invocable<decltype(task), Result, Arg>>(executor, std::move(task));
            }
        };

        /// ArityChecker implementation
        /// ========================================================================================

        template <typename Expected, typename Right, bool = core::IsTuple<Expected>::value>
        struct ArityCheckerBase;

        template <typename Expected, typename Right>
        struct ArityCheckerBase<Expected, Right, true>
        {
            static constexpr void validate()
            {
                static_assert(std::tuple_size_v<Expected> == std::tuple_size_v<Right>, "Wrong number of arguments");
                static_assert(std::is_same_v<Expected, Right>, "Wrong argument types");
            }
        };

        template <typename Expected, typename Right>
        struct ArityCheckerBase<Expected, Right, false>
            : public ArityCheckerBase<std::conditional_t<std::is_same_v<Expected, void>, std::tuple<>, std::tuple<Expected>>, Right>
        {
        };

        template <typename Expected, typename Provided>
        struct ArityChecker;

        template <typename Expected, typename... Provided>
        struct ArityChecker<Expected, std::tuple<Provided...>>: public ArityCheckerBase<Expected, std::tuple<Provided...>>
        {
        };

        template <typename Expected, typename... Provided>
        struct ArityChecker<Expected, std::tuple<CancellationToken, Provided...>>
            : public ArityCheckerBase<Expected, std::tuple<Provided...>>
        {
        };

        template <typename Expected, typename... Provided>
        struct ArityChecker<Expected, std::tuple<std::exception, Provided...>>: public ArityCheckerBase<Expected, std::tuple<Provided...>>
        {
        };

        template <typename Expected, typename... Provided>
        struct ArityChecker<Expected, std::tuple<std::exception, CancellationToken, Provided...>>
            : public ArityCheckerBase<Expected, std::tuple<Provided...>>
        {
        };
    } // namespace internal

    /// Task::Builder implementation
    /// ============================================================================================

    template <typename Signature>
    template <typename TaskFunc, typename... Args, typename TaskBuilder>
    TaskBuilder Task::Builder<Signature>::then(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        internal::ArityChecker<typename std::decay<ResultType>::type, typename TaskBuilder::PureArgs>::validate();
        constexpr bool HasToken = std::is_same_v<typename std::decay<typename TaskBuilder::FirstArg>::type, CancellationToken>;

        auto invocable = internal::BuilderHelper<typename TaskBuilder::ResultType, ResultType, HasToken>::createInvocable(
            executor, _task->getToken(), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();

        _last->setNext(std::move(invocable));
        return TaskBuilder{std::move(_task), last};
    }

    /// Task implementation
    /// ============================================================================================

    template <typename TaskFunc, typename... Args, typename TaskBuilder>
    TaskBuilder Task::run(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        constexpr bool HasToken = std::is_same_v<typename std::decay<typename TaskBuilder::FirstArg>::type, CancellationToken>;

        auto token     = CancellationToken();
        auto invocable = internal::BuilderHelper<typename TaskBuilder::ResultType, void, HasToken>::createInvocable(
            executor, token, std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();
        invocable->setToken(std::move(token));

        return TaskBuilder{std::move(invocable), last};
    }

} // namespace vanilo::tasker

#endif // INC_CF1B33A15FDE47BDA967EACB24A90BED