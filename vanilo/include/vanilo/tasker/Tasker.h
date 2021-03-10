#ifndef INC_CF1B33A15FDE47BDA967EACB24A90BED
#define INC_CF1B33A15FDE47BDA967EACB24A90BED

#include <vanilo/Export.h>
#include <vanilo/core/Binder.h>
#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/CancellationToken.h>

#include <any>
#include <cassert>
#include <future>

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
        template <typename Invocable, typename Signature, typename Arg, bool Primary = true>
        class Builder;

      public:
        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<void, TaskFunc, void>>
        static auto run(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<void, TaskFunc, void>>
        static auto run(TaskExecutor* executor, CancellationToken token, TaskFunc&& func, Args&&... args);

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

    /// LocalThreadExecutor interface
    /// ============================================================================================

    class VANILO_EXPORT LocalThreadExecutor: public TaskExecutor
    {
      public:
        static std::unique_ptr<LocalThreadExecutor> create();

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
            BaseTaskBuilder(std::unique_ptr<internal::ChainableTask> task, internal::ChainableTask* current, internal::ChainableTask* last);

            std::unique_ptr<internal::ChainableTask> _task;
            internal::ChainableTask* _current;
            internal::ChainableTask* _last;
        };
    } // namespace internal

    /// Task::Builder declaration
    /// ============================================================================================

    template <typename Invocable, typename Signature, typename Arg, bool Primary>
    class Task::Builder: public internal::BaseTaskBuilder
    {
        friend Task;

      public:
        using ResultType = typename core::traits::FunctionTraits<Signature>::ReturnType;
        using FirstArg   = typename core::traits::FunctionTraits<Signature>::template Arg<0>;
        using PureArgs   = typename core::traits::FunctionTraits<Signature>::PureArgsType;

        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<void, TaskFunc, Arg>>
        auto then(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<void, TaskFunc, Arg>>
        auto onException(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

        auto getFuture() -> std::future<ResultType>;

      private:
        using BaseTaskBuilder::BaseTaskBuilder;
    };

    namespace internal {

        /// ArityChecker implementation
        /// ========================================================================================

        template <typename Expected, typename Provided, bool = core::IsTuple<Expected>::value>
        struct ArityCheckerBase;

        template <typename Expected, typename Provided>
        struct ArityCheckerBase<Expected, Provided, true>
        {
            static constexpr size_t ExpectedArity = std::tuple_size_v<Expected>;
            static constexpr size_t ProvidedArity = std::tuple_size_v<Provided>;

            static constexpr void validate()
            {
                static_assert(std::tuple_size_v<Expected> == std::tuple_size_v<Provided>, "Wrong number of arguments");
                static_assert(std::is_same_v<Expected, Provided>, "Wrong argument types");
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

        /**
         * Generic task specialization.
         * @tparam Callable The type of the task
         * @tparam Result The result type returned by the task
         * @tparam Arg The type of the argument which is taken by the task
         * @tparam Promised Boolean flag to choose between normal and promised task
         */
        template <typename Callable, typename Result, typename Arg, bool Promised = false>
        class Invocable;

        template <typename Callable, typename Result, typename Arg>
        class BaseTask;

        template <typename Callable, typename Result, typename Arg>
        class PromisedTask;

        /**
         * Represents an abstract generic chainable task.
         */
        class ChainableTask: public Task
        {
            template <typename C, typename R, typename A>
            friend class BaseTask;

          public:
            explicit ChainableTask(TaskExecutor* executor): _executor{executor}
            {
            }

            ChainableTask(ChainableTask&& other) noexcept
                : _executor{other._executor}, _token{std::move(other._token)}, _next{std::move(other._next)}
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

            [[nodiscard]] ChainableTask* getLastTask()
            {
                auto task = this;

                while (task->_next) {
                    task = task->_next.get();
                }

                return task;
            }

          protected:
            [[nodiscard]] virtual bool isPromised() const noexcept = 0;
            virtual void handleException(std::exception_ptr exPtr) = 0;

            inline void scheduleNext()
            {
                _next->_token = std::move(_token);
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
            explicit ParameterizedChainableTask(TaskExecutor* executor): ChainableTask{executor}
            {
            }

            ParameterizedChainableTask(ParameterizedChainableTask&& other) noexcept
                : ChainableTask{std::move(other)}, _param{std::move(other._param)}
            {
            }

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
            template <typename C, typename R, typename A>
            friend class PromisedTask;

            using ErrorHandler = bool (*)(TaskExecutor* executor, CancellationToken&, Callable&, std::any&, const std::exception_ptr&);

          public:
            explicit BaseTask(TaskExecutor* executor, Callable&& task)
                : ParameterizedChainableTask<Arg>{executor}, _task{std::move(task)}, _errorExecutor{nullptr},
                  _errorHandler{[](TaskExecutor* executor, CancellationToken&, Callable&, std::any&, const std::exception_ptr&) {
                      return false; // Exception was not handled
                  }}
            {
            }

            template <typename Functor, typename... Args>
            void setupExceptionCallback(TaskExecutor* executor, Functor&& functor, Args&&... args)
            {
                _errorExecutor = executor;
                _errorMetadata = std::make_any<std::tuple<Functor, std::tuple<Args...>>>(
                    std::make_tuple(std::forward<Functor>(functor), std::forward_as_tuple(std::forward<Args>(args)...)));

                _errorHandler = [isSameExecutor = this->_executor == executor](
                                    TaskExecutor* executor, CancellationToken& token, Callable& task, std::any& metadata,
                                    const std::exception_ptr& exPtr) {
                    auto exceptionTask = core::binder::bind(
                        [](CancellationToken& token, Callable& task, std::any& metadata, std::exception_ptr& exPtr) {
                            auto [func, args] = std::move(std::any_cast<std::tuple<Functor, std::tuple<Args...>>>(metadata));

                            try {
                                try {
                                    std::rethrow_exception(exPtr);
                                }
                                catch (std::exception& ex) {
                                    using TokenArg =
                                        typename std::decay<typename core::traits::FunctionTraits<Functor>::template Arg<1>>::type;
                                    constexpr bool HasToken = std::is_same_v<TokenArg, CancellationToken>;

                                    // Pack exception and optional cancellation token
                                    auto args2 = std::tuple<std::reference_wrapper<std::exception>, CancellationToken>(
                                        std::ref(ex), std::move(token));

                                    rebindAndInvokeCallable(
                                        task, std::forward<Functor>(func), args,
                                        std::make_index_sequence<std::tuple_size_v<decltype(args)>>{}, args2,
                                        std::make_index_sequence<1 + HasToken>{});
                                }
                            }
                            catch (...) {
                                /// Log the error coming form the exception handler callback
                                TRACE("An unexpected exception occurred during execution of onException callback!");
                            }
                        },
                        std::move(token), std::move(task), std::move(metadata), exPtr);

                    // If another executor is provided where the exception has to be handled
                    if (isSameExecutor) {
                        exceptionTask();
                    }
                    else {
                        executor->submit(
                            std::make_unique<internal::Invocable<decltype(exceptionTask), void, void>>(executor, std::move(exceptionTask)));
                    }

                    return true; // Exception was handled
                };
            }

            [[nodiscard]] auto toPromisedTask() noexcept
            {
                return std::make_unique<Invocable<Callable, Result, Arg, true>>(std::move(*this));
            }

          protected:
            [[nodiscard]] bool isPromised() const noexcept override
            {
                return false;
            }

            void handleException(std::exception_ptr exPtr) override
            {
                auto lastTask = this->getLastTask();

                if (lastTask->isPromised()) {
                    lastTask->handleException(std::move(exPtr));
                    return;
                }

                if (!_errorHandler(_errorExecutor, this->_token, _task, _errorMetadata, exPtr)) {
                    TRACE("An unhandled exception occurred!");
                }
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
            template <typename Functor, typename Args1, std::size_t... Indexes1, typename Args2, std::size_t... Indexes2>
            inline static void rebindAndInvokeCallable(
                Callable& task,
                Functor&& func,
                Args1& args1,
                std::index_sequence<Indexes1...>,
                Args2& args2,
                std::index_sequence<Indexes2...>)
            {
                using BoundedTokenArg = typename std::decay<typename std::decay<decltype(task)>::type::template Element<0>>::type;
                using ProvidedArgs    = typename core::traits::FunctionTraits<Functor>::PureArgsType;

                constexpr bool HasBoundedToken = std::is_same_v<BoundedTokenArg, CancellationToken>;
                constexpr auto SelectedArgNum  = ArityChecker<void, ProvidedArgs>::ProvidedArity;

                task.template rebindSelectedPrepend<HasBoundedToken, SelectedArgNum>(
                    std::forward<Functor>(func), std::move(std::get<Indexes1>(args1))..., std::move(std::get<Indexes2>(args2))...)();
            }

          private:
            Callable _task;
            TaskExecutor* _errorExecutor{};
            std::any _errorMetadata{};
            ErrorHandler _errorHandler;
        };

        template <typename Callable, typename Result, typename Arg>
        class PromisedTask: public ParameterizedChainableTask<Arg>
        {
          public:
            explicit PromisedTask(TaskExecutor* executor, Callable&& task)
                : ParameterizedChainableTask<Arg>{executor}, _task{std::move(task)}
            {
            }

            [[nodiscard]] std::future<Result> getFuture()
            {
                return _promise.get_future();
            }

          protected:
            explicit PromisedTask(BaseTask<Callable, Result, Arg>&& other) noexcept
                : ParameterizedChainableTask<Arg>{std::forward<BaseTask<Callable, Result, Arg>>(other)}, _task{std::move(other._task)}
            {
            }

            [[nodiscard]] bool isPromised() const noexcept override
            {
                return true;
            }

            void handleException(std::exception_ptr exPtr) override
            {
                _promise.set_exception(std::move(exPtr));
            }

            template <typename T = Arg, typename std::enable_if_t<std::is_void_v<T>, bool> = true>
            inline Result executeTask()
            {
                _promise.set_value(_task());
                return Result{}; // Since PromisedTask is the last one this value will never be used
            }

            template <typename T = Arg, typename std::enable_if_t<!std::is_void_v<T>, bool> = true>
            inline Result executeTask()
            {
                _promise.set_value(invoke(this->_param));
                return Result{}; // Since PromisedTask is the last one this value will never be used
            }

          private:
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

            Callable _task;
            std::promise<Result> _promise{};
        };

        template <typename Callable, typename Arg>
        class PromisedTask<Callable, void, Arg>: public ParameterizedChainableTask<Arg>
        {
          public:
            explicit PromisedTask(TaskExecutor* executor, Callable&& task)
                : ParameterizedChainableTask<Arg>{executor}, _task{std::move(task)}
            {
            }

            [[nodiscard]] std::future<void> getFuture()
            {
                return _promise.get_future();
            }

            [[nodiscard]] bool isPromised() const noexcept override
            {
                return true;
            }

          protected:
            explicit PromisedTask(BaseTask<Callable, void, Arg>&& other) noexcept
                : ParameterizedChainableTask<Arg>{std::forward<BaseTask<Callable, void, Arg>>(other)}, _task{std::move(other._task)}
            {
            }

            void handleException(std::exception_ptr exPtr) override
            {
                // PromisedTask is supposed to be the last one in the chain
                _promise.set_exception(std::move(exPtr));
            }

            template <typename T = Arg, typename std::enable_if_t<std::is_void_v<T>, bool> = true>
            inline void executeTask()
            {
                _task();
                _promise.set_value();
            }

            template <typename T = Arg, typename std::enable_if_t<!std::is_void_v<T>, bool> = true>
            inline void executeTask()
            {
                invoke(this->_param);
                _promise.set_value();
            }

          private:
            template <typename Param, typename = std::enable_if_t<!core::IsTuple<Param>::value>>
            inline void invoke(Param& param)
            {
                _task(param);
            }

            template <typename... Args, typename Packed = std::tuple<Args...>, typename = std::enable_if_t<core::IsTuple<Packed>::value>>
            inline void invoke(std::tuple<Args...>& args)
            {
                core::InvokeHelper<void>::invoke(_task, args, std::make_index_sequence<sizeof...(Args)>{});
            }

            Callable _task;
            std::promise<void> _promise{};
        };

        template <typename Callable, typename Result, typename Arg, bool Promised>
        using BaseInvocable = std::conditional_t<Promised, PromisedTask<Callable, Result, Arg>, BaseTask<Callable, Result, Arg>>;

        template <typename Callable, typename Result, typename Arg, bool Promised>
        class Invocable: public BaseInvocable<Callable, Result, Arg, Promised>
        {
          public:
            explicit Invocable(TaskExecutor* executor, Callable&& task)
                : BaseInvocable<Callable, Result, Arg, Promised>(executor, std::move(task))
            {
            }

            explicit Invocable(BaseInvocable<Callable, Result, Arg, false>&& other) noexcept
                : BaseInvocable<Callable, Result, Arg, true>{std::forward<BaseInvocable<Callable, Result, Arg, false>>(other)}
            {
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

        template <typename Callable, typename Result, bool Promised>
        class Invocable<Callable, Result, void, Promised>: public BaseInvocable<Callable, Result, void, Promised>
        {
          public:
            explicit Invocable(TaskExecutor* executor, Callable&& task)
                : BaseInvocable<Callable, Result, void, Promised>(executor, std::move(task))
            {
            }

            explicit Invocable(BaseInvocable<Callable, Result, void, false>&& other) noexcept
                : BaseInvocable<Callable, Result, void, true>{std::forward<BaseInvocable<Callable, Result, void, false>>(other)}
            {
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

        template <typename Callable, typename Arg, bool Promised>
        class Invocable<Callable, void, Arg, Promised>: public BaseInvocable<Callable, void, Arg, Promised>
        {
          public:
            explicit Invocable(TaskExecutor* executor, Callable&& task)
                : BaseInvocable<Callable, void, Arg, Promised>(executor, std::move(task))
            {
            }

            explicit Invocable(BaseInvocable<Callable, void, Arg, false>&& other) noexcept
                : BaseInvocable<Callable, void, Arg, true>{std::forward<BaseInvocable<Callable, void, Arg, false>>(other)}
            {
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

        template <typename Callable, bool Promised>
        class Invocable<Callable, void, void, Promised>: public BaseInvocable<Callable, void, void, Promised>
        {
          public:
            explicit Invocable(TaskExecutor* executor, Callable&& task)
                : BaseInvocable<Callable, void, void, Promised>(executor, std::move(task))
            {
            }

            explicit Invocable(BaseInvocable<Callable, void, void, false>&& other) noexcept
                : BaseInvocable<Callable, void, void, true>{std::forward<BaseInvocable<Callable, void, void, false>>(other)}
            {
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

        /// BuilderHelper implementation
        /// ========================================================================================

        template <typename Result, typename Arg, bool IsMember, bool HasToken>
        struct BuilderHelper;

        template <typename Result, typename Arg, bool IsMember>
        struct BuilderHelper<Result, Arg, IsMember, false>
        {
            template <typename TaskFunc, typename... Args>
            static auto createInvocable(TaskExecutor* executor, CancellationToken token, TaskFunc&& func, Args&&... args)
            {
                auto task      = core::binder::bind(std::forward<TaskFunc>(func), std::forward<Args>(args)...);
                auto invocable = std::make_unique<internal::Invocable<decltype(task), Result, Arg>>(executor, std::move(task));
                invocable->setToken(std::move(token));
                return std::move(invocable);
            }
        };

        template <typename Result, typename Arg>
        struct BuilderHelper<Result, Arg, false, true>
        {
            template <typename TaskFunc, typename... Args>
            static auto createInvocable(TaskExecutor* executor, CancellationToken token, TaskFunc&& func, Args&&... args)
            {
                auto task      = core::binder::bind(std::forward<TaskFunc>(func), token, std::forward<Args>(args)...);
                auto invocable = std::make_unique<internal::Invocable<decltype(task), Result, Arg>>(executor, std::move(task));
                invocable->setToken(std::move(token));
                return std::move(invocable);
            }
        };

        template <typename Result, typename Arg>
        struct BuilderHelper<Result, Arg, true, true>
        {
            template <typename TaskFunc, typename Arg1, typename... Args>
            static auto createInvocable(TaskExecutor* executor, CancellationToken token, TaskFunc&& func, Arg1&& arg1, Args&&... args)
            {
                auto task = core::binder::bind(std::forward<TaskFunc>(func), std::forward<Arg1>(arg1), token, std::forward<Args>(args)...);
                auto invocable = std::make_unique<internal::Invocable<decltype(task), Result, Arg>>(executor, std::move(task));
                invocable->setToken(std::move(token));
                return std::move(invocable);
            }
        };

        /// BaseTaskBuilder implementation
        /// ========================================================================================

        inline BaseTaskBuilder::BaseTaskBuilder(
            std::unique_ptr<internal::ChainableTask> task, internal::ChainableTask* current, internal::ChainableTask* last)
            : _task{std::move(task)}, _current{current}, _last{last}
        {
        }

        inline BaseTaskBuilder::~BaseTaskBuilder()
        {
            if (_task) { // Schedule task on the executor
                _task->getExecutor()->submit(std::move(_task));
            }
        }

    } // namespace internal

    /// Task::Builder implementation
    /// ============================================================================================

    template <typename Invocable, typename Signature, typename Arg, bool Primary>
    template <typename TaskFunc, typename... Args, typename TaskBuilder>
    auto Task::Builder<Invocable, Signature, Arg, Primary>::then(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        internal::ArityChecker<typename std::decay<ResultType>::type, typename TaskBuilder::PureArgs>::validate();
        constexpr bool HasToken = std::is_same_v<typename std::decay<typename TaskBuilder::FirstArg>::type, CancellationToken>;
        constexpr bool IsMember = core::traits::FunctionTraits<TaskFunc>::IsMemberFnPtr;

        auto invocable = internal::BuilderHelper<typename TaskBuilder::ResultType, ResultType, IsMember, HasToken>::createInvocable(
            executor, _task->getToken(), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();

        _last->setNext(std::move(invocable));
        return Task::Builder<typename decltype(invocable)::element_type, Signature, Arg>{std::move(_task), _last, last};
    }

    template <typename Invocable, typename Signature, typename Arg, bool Primary>
    template <typename TaskFunc, typename... Args, typename TaskBuilder>
    auto Task::Builder<Invocable, Signature, Arg, Primary>::onException(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        auto task = static_cast<Invocable*>(_task->getLastTask());
        task->setupExceptionCallback(executor, std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        return Task::Builder<Invocable, Signature, Arg, false>{std::move(_task), _current, _last};
    }

    template <typename Invocable, typename Signature, typename Arg, bool Primary>
    auto Task::Builder<Invocable, Signature, Arg, Primary>::getFuture() -> std::future<ResultType>
    {
        auto task     = static_cast<Invocable*>(_task->getLastTask());
        auto promised = task->toPromisedTask();
        auto future   = promised->getFuture();

        if (_current == _last) {
            _task    = std::move(promised);
            _current = _last = _task.get();
        }
        else {
            _current->setNext(std::move(promised));
        }

        return future;
    }

    /// Task implementation
    /// ============================================================================================

    template <typename TaskFunc, typename... Args, typename TaskBuilder>
    auto Task::run(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        return Task::run(executor, CancellationToken{}, std::forward<TaskFunc>(func), std::forward<Args>(args)...);
    }

    template <typename TaskFunc, typename... Args, typename TaskBuilder>
    auto Task::run(TaskExecutor* executor, CancellationToken token, TaskFunc&& func, Args&&... args)
    {
        constexpr bool HasToken = std::is_same_v<typename std::decay<typename TaskBuilder::FirstArg>::type, CancellationToken>;
        constexpr bool IsMember = core::traits::FunctionTraits<TaskFunc>::IsMemberFnPtr;

        auto invocable = internal::BuilderHelper<typename TaskBuilder::ResultType, void, IsMember, HasToken>::createInvocable(
            executor, std::move(token), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();

        return Task::Builder<typename decltype(invocable)::element_type, TaskFunc, void>{std::move(invocable), last, last};
    }

} // namespace vanilo::tasker

#endif // INC_CF1B33A15FDE47BDA967EACB24A90BED