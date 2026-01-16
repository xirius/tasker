#ifndef INC_CF1B33A15FDE47BDA967EACB24A90BED
#define INC_CF1B33A15FDE47BDA967EACB24A90BED

#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/core/Binder.h>
#include <vanilo/core/Tracer.h>

#include <any>
#include <cassert>
#include <future>
#include <optional>

namespace vanilo::tasker {

    /**
     * Object that manages the execution of the scheduled tasks.
     */
    class TaskExecutor;

    /// LifeCycleGuard | Atomic Gate
    /// ============================================================================================

    class LifeCycleGuard
    {
      public:
        /**
         * Attempts to increment the internal reference counter.
         * If the guard is marked as "terminated," the method denies increment and returns false.
         * Uses atomic operations to ensure thread safety and provides a double-check mechanism
         * to handle potential races with the `terminate()` method.
         *
         * @return True if the increment is successful; false if the guard is terminated.
         */
        bool tryIncrement()
        {
            if (_isTerminated.load(std::memory_order_acquire)) {
                return false;
            }

            _refs.fetch_add(1, std::memory_order_relaxed);

            // Double-check after incrementing to handle the race with terminate()
            if (_isTerminated.load(std::memory_order_acquire)) {
                decrement();
                return false;
            }

            return true;
        }

        /**
         * Decrements the internal reference counter. This is used to track when
         * an execution context releases the guard.
         */
        void decrement()
        {
            _refs.fetch_sub(1, std::memory_order_release);
        }

        /**
         * Terminates the guard by setting an internal flag, indicating that no new increments
         * should be allowed. Waits until all currently active references have dropped to zero before
         * returning, ensuring that the guarded resource is fully drained.
         */
        void terminate()
        {
            _isTerminated.store(true, std::memory_order_release);
            // Spin-wait until all in-flight entries are finished
            while (_refs.load(std::memory_order_acquire) > 0) {
                std::this_thread::yield();
            }
        }

        /**
         * RAII wrapper for guard acquisition.
         */
        struct Lock
        {
            friend class LifeCycleGuard;

            Lock() = default;

            Lock(Lock&& other) noexcept: _guard{other._guard}
            {
                other._guard = nullptr;
            }

            Lock& operator=(Lock&& other) noexcept
            {
                if (this != &other) {
                    if (_guard) {
                        _guard->decrement();
                    }
                    _guard = other._guard;
                    other._guard = nullptr;
                }
                return *this;
            }

            ~Lock()
            {
                if (_guard) {
                    _guard->decrement();
                }
            }

            explicit operator bool() const
            {
                return _guard != nullptr;
            }

            Lock(const Lock&) = delete;
            Lock& operator=(const Lock&) = delete;

          private:
            explicit Lock(LifeCycleGuard* guard): _guard{guard}
            {
            }

            LifeCycleGuard* _guard{nullptr};
        };

        /**
         * Attempts to acquire the lock using RAII.
         * @return A Lock object that evaluates to true if acquisition was successful, false otherwise.
         */
        Lock acquire()
        {
            if (tryIncrement()) {
                return Lock{this};
            }
            return Lock{};
        }

      private:
        std::atomic<int> _refs{0};
        std::atomic<bool> _isTerminated{false};
    };

    /// Task interface
    /// ============================================================================================

    class VANILO_EXPORT Task
    {
        template <typename Invocable, typename Signature, typename Arg, bool Primary = true>
        class Builder;

        template <typename Invocable, typename Signature, typename Arg>
        class PeriodicBuilder;

      public:
        using CancellationToken = concurrent::CancellationToken;

        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<void, TaskFunc, void>>
        static auto run(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<void, TaskFunc, void>>
        static auto run(TaskExecutor* executor, CancellationToken token, TaskFunc&& func, Args&&... args);

        template <class Rep, class Period, typename TaskFunc, typename... Args, typename TaskBuilder = Builder<void, TaskFunc, void>>
        static auto run(
            TaskExecutor* executor, CancellationToken token, std::chrono::duration<Rep, Period> delay, TaskFunc&& func, Args&&... args);

        template <
            class Rep1,
            class Period1,
            class Rep2,
            class Period2,
            typename TaskFunc,
            typename... Args,
            typename TaskBuilder = Builder<void, TaskFunc, void>>
        static auto run(
            TaskExecutor* executor,
            CancellationToken token,
            std::chrono::duration<Rep1, Period1> initialDelay,
            std::chrono::duration<Rep2, Period2> interval,
            TaskFunc&& func,
            Args&&... args);

        virtual ~Task() = default;

        virtual void cancel() noexcept = 0;
        virtual void run() = 0;
    };

    /// TaskExecutor interface
    /// ============================================================================================

    class TaskExecutor
    {
      public:
        TaskExecutor() = default;
        TaskExecutor(const TaskExecutor& other) = delete;

        virtual ~TaskExecutor()
        {
            _guard->terminate();
        }

        TaskExecutor& operator=(const TaskExecutor& other) = delete;

        /**
         * @return The current number of tasks in the queue.
         */
        [[nodiscard]] virtual size_t count() const = 0;

        /**
         * Retrieves the lifecycle guard associated with the task executor.
         * @return A shared pointer to the lifecycle guard.
         */
        [[nodiscard]] std::shared_ptr<LifeCycleGuard> getGuard() const
        {
            return _guard;
        }

        /**
         * Submits the task for the execution on the task executor.
         * @param task The task to be executed on the task executor.
         */
        virtual void submit(std::unique_ptr<Task> task) = 0;

      private:
        std::shared_ptr<LifeCycleGuard> _guard = std::make_shared<LifeCycleGuard>();
    };

    /// LocalThreadExecutor interface
    /// ============================================================================================

    class VANILO_EXPORT LocalThreadExecutor: public TaskExecutor
    {
      public:
        using CancellationToken = concurrent::CancellationToken;

        static std::unique_ptr<LocalThreadExecutor> create();

        /**
         * Processes max number of tasks in the queue if any present.
         * @param maxCount the maximum number of tasks to process.
         * @return The number of the unprocessed tasks in the queue.
         */
        virtual size_t process(size_t maxCount) = 0;

        /**
         * Processes the tasks in the queue until the provided token is canceled.
         * @param token The cancellation token.
         */
        virtual size_t process(const CancellationToken& token) = 0;
    };

    /// ThreadPoolExecutor interface
    /// ============================================================================================

    class VANILO_EXPORT ThreadPoolExecutor: public TaskExecutor
    {
      public:
        /**
         * The number of concurrent threads supported by the implementation.
         */
        static size_t DefaultThreadNumber;

        /**
         * Creates a new thread pool executor with the default number of threads.
         * @return The new instance of the default ThreadPoolExecutor.
         */
        static std::unique_ptr<ThreadPoolExecutor> create();

        /**
         * Creates a new thread pool executor with the desired number of threads.
         * @param numThreads The desired number of threads in the thread pool.
         * @return The new instance of the default ThreadPoolExecutor with the desired number of threads.
         */
        static std::unique_ptr<ThreadPoolExecutor> create(size_t numThreads);

        /**
         * Resizes the number of the threads in the thread pool.
         * @param numThreads The desired number of threads in the thread pool.
         * @return The future that is set when the resizing operation is finished.
         */
        virtual std::future<void> resize(size_t numThreads) = 0;

        /**
         * Checks if the thread pool contains a thread with the specified id.
         * @param threadId The id of the thread to check.
         * @return True if the thead pool contains a thread with the specified id; false otherwise.
         */
        [[nodiscard]] virtual bool containsThread(std::thread::id threadId) const = 0;

        /**
         * Returns the number of threads in the executor thread pool.
         * @return The number of threads in the thread pool.
         */
        [[nodiscard]] virtual size_t threadCount() const noexcept = 0;

        /**
         * Returns the list of the thread ids.
         * @return The list of the thread ids.
         */
        [[nodiscard]] virtual std::vector<std::thread::id> threadIds() const = 0;
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
            explicit BaseTaskBuilder(std::unique_ptr<ChainableTask> task);

          private:
            std::unique_ptr<ChainableTask> _task;
        };

        /// TaskHelper declaration
        /// ========================================================================================

        class TaskHelper
        {
          public:
            using steady_clock = std::chrono::steady_clock;
            using system_clock = std::chrono::system_clock;

            template <class Rep, class Period>
            static std::unique_ptr<ChainableTask> convertTask(
                std::unique_ptr<ChainableTask> task, std::chrono::duration<Rep, Period> delay);

            template <class Rep1, class Period1, class Rep2, class Period2>
            static std::unique_ptr<ChainableTask> convertTask(
                std::unique_ptr<ChainableTask> task,
                std::chrono::duration<Rep1, Period1> delay,
                std::chrono::duration<Rep2, Period2> interval);

          private:
            static std::unique_ptr<ChainableTask> convertTask(std::unique_ptr<ChainableTask> task, steady_clock::duration delay);
            static std::unique_ptr<ChainableTask> convertTask(
                std::unique_ptr<ChainableTask> task, steady_clock::duration delay, steady_clock::duration interval);
        };
    } // namespace internal

    /// Task::Builder declaration
    /// ============================================================================================

    template <typename Invocable, typename Signature, typename Arg>
    class Task::Builder<Invocable, Signature, Arg, false>: public internal::BaseTaskBuilder
    {
        friend Task;

      public:
        using ResultType = typename core::traits::FunctionTraits<Signature>::ReturnType;
        using FirstArg = typename core::traits::FunctionTraits<Signature>::template Arg<0>;
        using PureArgs = typename core::traits::FunctionTraits<Signature>::PureArgsType;

        template <typename TaskFunc, typename... Args, typename TaskBuilder = Builder<void, TaskFunc, Arg>>
        auto then(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

      protected:
        Builder(std::unique_ptr<internal::ChainableTask> task, internal::ChainableTask* current, internal::ChainableTask* last);

        internal::ChainableTask* _current;
        internal::ChainableTask* _last;
    };

    template <typename Invocable, typename Signature, typename Arg>
    class Task::Builder<Invocable, Signature, Arg, true> final: public Builder<Invocable, Signature, Arg, false>
    {
        friend Task;

        template <typename T>
        using IsExecutor = std::is_base_of<TaskExecutor, std::remove_pointer_t<std::decay_t<T>>>;

        template <typename Functor>
        using TaskBuilder = Builder<Invocable, Functor, Arg, false>;

      public:
        using Return = typename Builder<Invocable, Signature, Arg, false>::ResultType;

        auto getFuture() -> std::future<Return>;

        template <typename TaskFunc, typename... Args>
        auto onException(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

        template <typename TaskFunc, typename... Args, typename = std::enable_if_t<!IsExecutor<TaskFunc>::value>>
        auto onException(TaskFunc&& func, Args&&... args);

      private:
        using Builder<Invocable, Signature, Arg, false>::Builder;
    };

    template <typename Invocable, typename Signature, typename Arg>
    class Task::PeriodicBuilder final: public Builder<Invocable, Signature, Arg, false>
    {
        friend Task;

        template <typename T>
        using IsExecutor = std::is_base_of<TaskExecutor, std::remove_pointer_t<std::decay_t<T>>>;

        template <typename Functor>
        using TaskBuilder = PeriodicBuilder<Invocable, Functor, Arg>;

      public:
        template <typename TaskFunc, typename... Args, typename TaskBuilder = PeriodicBuilder<void, TaskFunc, Arg>>
        auto then(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

        template <typename TaskFunc, typename... Args>
        auto onException(TaskExecutor* executor, TaskFunc&& func, Args&&... args);

        template <typename TaskFunc, typename... Args, typename = std::enable_if_t<!IsExecutor<TaskFunc>::value>>
        auto onException(TaskFunc&& func, Args&&... args);

      private:
        using Builder<Invocable, Signature, Arg, false>::Builder;
    };

    namespace internal {
        using CancellationToken = concurrent::CancellationToken;

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
            : ArityCheckerBase<std::conditional_t<std::is_same_v<Expected, void>, std::tuple<>, std::tuple<Expected>>, Right>
        {
        };

        template <typename Expected, typename Provided>
        struct ArityChecker;

        template <typename Expected, typename... Provided>
        struct ArityChecker<Expected, std::tuple<Provided...>>: ArityCheckerBase<Expected, std::tuple<Provided...>>
        {
        };

        template <typename Expected, typename... Provided>
        struct ArityChecker<Expected, std::tuple<CancellationToken, Provided...>>: ArityCheckerBase<Expected, std::tuple<Provided...>>
        {
        };

        template <typename Expected, typename... Provided>
        struct ArityChecker<Expected, std::tuple<std::exception, Provided...>>: ArityCheckerBase<Expected, std::tuple<Provided...>>
        {
        };

        template <typename Expected, typename... Provided>
        struct ArityChecker<Expected, std::tuple<std::exception, CancellationToken, Provided...>>
            : ArityCheckerBase<Expected, std::tuple<Provided...>>
        {
        };

        /**
         * Generic task specialization.
         * @tparam Callable The type of the task
         * @tparam Result The result type returned by the task
         * @tparam Arg The type of the argument which is taken by the task
         * @tparam Promised Boolean flag to choose between a normal and promised task
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
            explicit ChainableTask(TaskExecutor* executor): _executor{executor}, _guard{executor ? executor->getGuard() : nullptr}
            {
            }

            ChainableTask(ChainableTask&& other) noexcept
                : _cancelled{other._cancelled.load()}, _executor{other._executor}, _guard{std::move(other._guard)},
                  _token{std::move(other._token)}, _next{std::move(other._next)}
            {
            }

            ~ChainableTask() override = default;

            [[nodiscard]] virtual std::unique_ptr<ChainableTask> clone() const = 0;

            [[nodiscard]] std::unique_ptr<ChainableTask> cloneChain(const bool skipSelf) const
            {
                if (skipSelf) {
                    return _next ? _next->cloneChain(false) : nullptr;
                }

                auto cloned = clone();
                if (cloned && _next) {
                    if (auto clonedNext = _next->cloneChain(false)) {
                        cloned->setNext(std::move(clonedNext));
                    }
                }
                return cloned;
            }

            void cancel() noexcept override
            {
                _cancelled.store(true, std::memory_order_release);
            }

            [[nodiscard]] LifeCycleGuard::Lock acquireGuard() const
            {
                if (_guard) {
                    return _guard->acquire();
                }
                return {};
            }

            [[nodiscard]] bool isCanceled() const noexcept
            {
                return _cancelled.load(std::memory_order_acquire);
            }

            [[nodiscard]] TaskExecutor* getExecutor() const
            {
                return _executor;
            }

            [[nodiscard]] CancellationToken getToken() const
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
            ChainableTask(TaskExecutor* executor, std::shared_ptr<LifeCycleGuard> guard): _executor{executor}, _guard{std::move(guard)}
            {
            }

            [[nodiscard]] virtual bool isPromised() const noexcept = 0;
            virtual void handleException(std::exception_ptr exPtr) = 0;

            [[nodiscard]] std::shared_ptr<LifeCycleGuard> getGuard() const
            {
                return _guard;
            }

            void scheduleNext()
            {
                if (!_next) {
                    return;
                }

                if (_next->isCanceled() || _next->getToken().isCancellationRequested()) {
                    _next.reset();
                    return;
                }

                if (const auto nextGuard = _next->_guard) {
                    if (const auto lock = nextGuard->acquire()) {
                        const auto executor = _next->_executor;
                        _next->_token = std::move(_token);
                        executor->submit(std::move(_next));
                        return;
                    }
                }

                // Task is canceled if it cannot be scheduled due to executor destruction
                _next->getLastTask()->handleException(std::make_exception_ptr(concurrent::OperationCanceledException()));
                _next.reset();
            }

            [[nodiscard]] bool hasNext() const
            {
                return _next != nullptr;
            }

            template <typename NextTask>
            [[nodiscard]] NextTask nextTaskAs() const
            {
                return dynamic_cast<NextTask>(_next.get());
            }

            void throwIfCancellationRequested() const
            {
                _token.throwIfCancellationRequested();
            }

          private:
            std::atomic_bool _cancelled{false};
            TaskExecutor* _executor;
            std::shared_ptr<LifeCycleGuard> _guard;
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

            ParameterizedChainableTask(TaskExecutor* executor, std::shared_ptr<LifeCycleGuard> guard)
                : ChainableTask{executor, std::move(guard)}
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
         * Represents a specialization of an abstract generic chainable task without a parameter.
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

            using ErrorHandler =
                bool (*)(TaskExecutor* executor, const CancellationToken&, Callable&, const std::any&, const std::exception_ptr&);

          public:
            explicit BaseTask(TaskExecutor* executor, Callable&& task)
                : ParameterizedChainableTask<Arg>{executor}, _task{std::move(task)}, _errorHandler{nullptr}
            {
            }

            BaseTask(const BaseTask& other)
                : ParameterizedChainableTask<Arg>{other.getExecutor(), other.getGuard()}, _task{other._task},
                  _errorExecutor{other._errorExecutor}, _errorMetadata{other._errorMetadata}, _errorHandler{other._errorHandler}
            {
                this->setToken(other.getToken());
            }

            template <typename Functor, typename... Args>
            void setupExceptionCallback(TaskExecutor* executor, Functor&& functor, Args&&... args)
            {
                _errorExecutor = this->_executor == executor ? nullptr : executor;
                _errorMetadata = std::make_any<std::tuple<Functor, std::tuple<Args...>>>(
                    std::make_tuple(std::forward<Functor>(functor), std::forward_as_tuple(std::forward<Args>(args)...)));
                _errorHandler = &BaseTask::errorHandlerThunk<Functor, Args...>;
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
                if (auto lastTask = this->getLastTask(); lastTask->isPromised()) {
                    lastTask->handleException(std::move(exPtr));
                    return;
                }

                if (_errorHandler && _errorHandler(_errorExecutor, this->getToken(), _task, _errorMetadata, exPtr)) {
                    return;
                }

                // The task did not handle exception, so it is rethrown
                std::rethrow_exception(exPtr);
            }

            template <typename T = Arg, std::enable_if_t<std::is_void_v<T>, std::nullptr_t> = nullptr>
            Result executeTask()
            {
                if (this->isCanceled()) {
                    throw concurrent::OperationCanceledException();
                }

                this->_token.throwIfCancellationRequested();
                return _task();
            }

            template <typename T = Arg, std::enable_if_t<!std::is_void_v<T>, std::nullptr_t> = nullptr>
            Result executeTask()
            {
                if (this->isCanceled()) {
                    throw concurrent::OperationCanceledException();
                }

                this->_token.throwIfCancellationRequested();
                return invoke(this->_param);
            }

            template <typename Param, typename = std::enable_if_t<!core::IsTuple<Param>::value>>
            Result invoke(Param& param)
            {
                return _task(param);
            }

            template <typename... Args, typename Packed = std::tuple<Args...>, typename = std::enable_if_t<core::IsTuple<Packed>::value>>
            Result invoke(std::tuple<Args...>& args)
            {
                return core::InvokeHelper<Result>::invoke(_task, args, std::make_index_sequence<sizeof...(Args)>{});
            }

          private:
            template <typename Functor, typename Args1, std::size_t... Indexes1, typename Args2, std::size_t... Indexes2>
            static void rebindAndInvokeCallable(
                Callable& task,
                Functor&& func,
                Args1& args1,
                std::index_sequence<Indexes1...>,
                Args2& args2,
                std::index_sequence<Indexes2...>)
            {
                using BoundedTokenArg = std::decay_t<typename std::decay_t<decltype(task)>::template Element<0>>;
                using ProvidedArgs = typename core::traits::FunctionTraits<Functor>::PureArgsType;

                constexpr bool HasBoundedToken = std::is_same_v<BoundedTokenArg, CancellationToken>;
                constexpr auto SelectedArgNum = ArityChecker<void, ProvidedArgs>::ProvidedArity;

                task.template rebindSelectedPrepend<HasBoundedToken, SelectedArgNum>(
                    std::forward<Functor>(func), std::move(std::get<Indexes1>(args1))..., std::move(std::get<Indexes2>(args2))...)();
            }

            template <typename FunctorT, typename... ArgsT>
            static bool errorHandlerThunk(
                TaskExecutor* currentExecutor,
                const CancellationToken& token,
                Callable& task,
                const std::any& metadata,
                const std::exception_ptr& exPtr)
            {
                auto exceptionTask =
                    core::binder::bind(&BaseTask::exceptionHandlerBody<FunctorT, ArgsT...>, token, std::move(task), metadata, exPtr);

                if (currentExecutor != nullptr) {
                    currentExecutor->submit(
                        std::make_unique<Invocable<decltype(exceptionTask), void, void>>(currentExecutor, std::move(exceptionTask)));
                }
                else {
                    exceptionTask();
                }

                return true; // Exception was handled
            }

            template <typename FunctorT, typename... ArgsT>
            static void exceptionHandlerBody(
                CancellationToken& innerToken, Callable& innerTask, std::any& innerMetadata, const std::exception_ptr& innerExPtr)
            {
                auto [func, args1] = std::move(std::any_cast<std::tuple<FunctorT, std::tuple<ArgsT...>>>(innerMetadata));

                try {
                    try {
                        std::rethrow_exception(innerExPtr);
                    }
                    catch (std::exception& ex) {
                        using TokenArg = std::decay_t<typename core::traits::FunctionTraits<FunctorT>::template Arg<1>>;
                        constexpr bool HasToken = std::is_same_v<TokenArg, CancellationToken>;

                        // Pack exception and optional cancellation token
                        auto args2 = std::tuple(std::ref(ex), std::move(innerToken));

                        rebindAndInvokeCallable(
                            innerTask, std::forward<FunctorT>(func), args1, std::make_index_sequence<std::tuple_size_v<decltype(args1)>>{},
                            args2, std::make_index_sequence<1 + HasToken>{});
                    }
                }
                catch (...) {
                    TRACE("An unexpected exception occurred during execution of onException callback!");
                }
            }

            Callable _task;
            TaskExecutor* _errorExecutor{};
            std::any _errorMetadata{};
            ErrorHandler _errorHandler =
                [](TaskExecutor* /*executor*/, const CancellationToken&, Callable&, const std::any&, const std::exception_ptr&) {
                return false; // Exception was not handled
            };
        };

        template <typename Callable, typename Result, typename Arg>
        class PromisedTask: public ParameterizedChainableTask<Arg>
        {
          public:
            explicit PromisedTask(TaskExecutor* executor, Callable&& task)
                : ParameterizedChainableTask<Arg>{executor}, _task{std::move(task)}
            {
            }

            PromisedTask(PromisedTask&& other) noexcept
                : ParameterizedChainableTask<Arg>{std::move(other)}, _task{std::move(other._task)}, _promise{std::move(other._promise)}
            {
            }

            ~PromisedTask() override = default;

            [[nodiscard]] std::future<Result> getFuture()
            {
                return _promise.get_future();
            }

          protected:
            explicit PromisedTask(BaseTask<Callable, Result, Arg>&& other) noexcept
                : ParameterizedChainableTask<Arg>{std::move(other)}, _task{std::move(other._task)}
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

            template <typename T = Arg, std::enable_if_t<std::is_void_v<T>, std::nullptr_t> = nullptr>
            void executeTask()
            {
                if (this->isCanceled()) {
                    throw concurrent::OperationCanceledException();
                }

                this->throwIfCancellationRequested();
                _promise.set_value(_task());
            }

            template <typename T = Arg, std::enable_if_t<!std::is_void_v<T>, std::nullptr_t> = nullptr>
            void executeTask()
            {
                if (this->isCanceled()) {
                    throw concurrent::OperationCanceledException();
                }

                this->throwIfCancellationRequested();
                _promise.set_value(invoke(this->_param));
            }

          private:
            template <typename Param, typename = std::enable_if_t<!core::IsTuple<Param>::value>>
            Result invoke(Param& param)
            {
                return _task(param);
            }

            template <typename... Args, typename Packed = std::tuple<Args...>, typename = std::enable_if_t<core::IsTuple<Packed>::value>>
            Result invoke(std::tuple<Args...>& args)
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

            PromisedTask(PromisedTask&& other) noexcept
                : ParameterizedChainableTask<Arg>{std::move(other)}, _task{std::move(other._task)}, _promise{std::move(other._promise)}
            {
            }

            ~PromisedTask() override = default;

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
                : ParameterizedChainableTask<Arg>{std::move(other)}, _task{std::move(other._task)}
            {
            }

            void handleException(std::exception_ptr exPtr) override
            {
                // PromisedTask is supposed to be the last one in the chain
                _promise.set_exception(std::move(exPtr));
            }

            template <typename T = Arg, std::enable_if_t<std::is_void_v<T>, std::nullptr_t> = nullptr>
            void executeTask()
            {
                if (this->isCanceled()) {
                    throw concurrent::OperationCanceledException();
                }

                this->throwIfCancellationRequested();
                _task();
                _promise.set_value();
            }

            template <typename T = Arg, std::enable_if_t<!std::is_void_v<T>, std::nullptr_t> = nullptr>
            void executeTask()
            {
                if (this->isCanceled()) {
                    throw concurrent::OperationCanceledException();
                }

                this->throwIfCancellationRequested();
                invoke(this->_param);
                _promise.set_value();
            }

          private:
            template <typename Param, typename = std::enable_if_t<!core::IsTuple<Param>::value>>
            void invoke(Param& param)
            {
                _task(param);
            }

            template <typename... Args, typename Packed = std::tuple<Args...>, typename = std::enable_if_t<core::IsTuple<Packed>::value>>
            void invoke(std::tuple<Args...>& args)
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
                : BaseInvocable<Callable, Result, Arg, true>{std::move(other)}
            {
            }

            [[nodiscard]] std::unique_ptr<ChainableTask> clone() const override
            {
                if constexpr (Promised) {
                    assert(false && "Promised tasks cannot be cloned");
                    return nullptr;
                }
                else {
                    return std::make_unique<Invocable>(*this);
                }
            }

            void run() override
            {
                try {
                    if constexpr (Promised) {
                        this->executeTask();
                    }
                    else {
                        if (this->hasNext()) {
                            auto next = this->template nextTaskAs<ParameterizedChainableTask<Result>*>();
                            next->setArgument(this->executeTask());
                            this->scheduleNext();
                        }
                        else {
                            this->executeTask();
                        }
                    }
                }
                catch (concurrent::OperationCanceledException& ex) {
                    this->handleException(std::make_exception_ptr(ex));
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
                : BaseInvocable<Callable, Result, void, true>{std::move(other)}
            {
            }

            [[nodiscard]] std::unique_ptr<ChainableTask> clone() const override
            {
                if constexpr (Promised) {
                    assert(false && "Promised tasks cannot be cloned");
                    return nullptr;
                }
                else {
                    return std::make_unique<Invocable>(*this);
                }
            }

            void run() override
            {
                try {
                    if constexpr (Promised) {
                        this->executeTask();
                    }
                    else {
                        if (this->hasNext()) {
                            auto next = this->template nextTaskAs<ParameterizedChainableTask<Result>*>();
                            next->setArgument(this->executeTask());
                            this->scheduleNext();
                        }
                        else {
                            this->executeTask();
                        }
                    }
                }
                catch (concurrent::OperationCanceledException& ex) {
                    this->handleException(std::make_exception_ptr(ex));
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
                : BaseInvocable<Callable, void, Arg, true>{std::move(other)}
            {
            }

            [[nodiscard]] std::unique_ptr<ChainableTask> clone() const override
            {
                if constexpr (Promised) {
                    assert(false && "Promised tasks cannot be cloned");
                    return nullptr;
                }
                else {
                    return std::make_unique<Invocable>(*this);
                }
            }

            void run() override
            {
                try {
                    this->executeTask();
                    this->scheduleNext();
                }
                catch (concurrent::OperationCanceledException& ex) {
                    this->handleException(std::make_exception_ptr(ex));
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
                : BaseInvocable<Callable, void, void, true>{std::move(other)}
            {
            }

            [[nodiscard]] std::unique_ptr<ChainableTask> clone() const override
            {
                if constexpr (Promised) {
                    assert(false && "Promised tasks cannot be cloned");
                    return nullptr;
                }
                else {
                    return std::make_unique<Invocable>(*this);
                }
            }

            void run() override
            {
                try {
                    this->executeTask();
                    this->scheduleNext();
                }
                catch (concurrent::OperationCanceledException& ex) {
                    this->handleException(std::make_exception_ptr(ex));
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
                auto task = core::binder::bind(std::forward<TaskFunc>(func), std::forward<Args>(args)...);
                auto invocable = std::make_unique<Invocable<decltype(task), Result, Arg>>(executor, std::move(task));
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
                auto task = core::binder::bind(std::forward<TaskFunc>(func), token, std::forward<Args>(args)...);
                auto invocable = std::make_unique<Invocable<decltype(task), Result, Arg>>(executor, std::move(task));
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
                auto invocable = std::make_unique<Invocable<decltype(task), Result, Arg>>(executor, std::move(task));
                invocable->setToken(std::move(token));
                return std::move(invocable);
            }
        };

        /// BaseTaskBuilder implementation
        /// ========================================================================================

        inline BaseTaskBuilder::BaseTaskBuilder(std::unique_ptr<ChainableTask> task): _task{std::move(task)}
        {
        }

        inline BaseTaskBuilder::~BaseTaskBuilder()
        {
            if (_task) { // Schedule task on the executor
                const auto executor = _task->getExecutor();
                executor->submit(std::move(_task));
            }
        }

        /// TaskHelper declaration
        /// ========================================================================================

        template <class Rep, class Period>
        std::unique_ptr<ChainableTask> TaskHelper::convertTask(
            std::unique_ptr<ChainableTask> task, std::chrono::duration<Rep, Period> delay)
        {
            return convertTask(std::move(task), std::chrono::duration_cast<steady_clock::duration>(delay));
        }

        template <class Rep1, class Period1, class Rep2, class Period2>
        std::unique_ptr<ChainableTask> TaskHelper::convertTask(
            std::unique_ptr<ChainableTask> task, std::chrono::duration<Rep1, Period1> delay, std::chrono::duration<Rep2, Period2> interval)
        {
            return convertTask(
                std::move(task), std::chrono::duration_cast<steady_clock::duration>(delay),
                std::chrono::duration_cast<steady_clock::duration>(interval));
        }
    } // namespace internal

    /// Task::Builder implementation
    /// ============================================================================================

    template <typename Invocable, typename Signature, typename Arg>
    Task::Builder<Invocable, Signature, Arg, false>::Builder(
        std::unique_ptr<internal::ChainableTask> task, internal::ChainableTask* current, internal::ChainableTask* last)
        : BaseTaskBuilder(std::move(task)), _current{current}, _last{last}
    {
    }

    template <typename Invocable, typename Signature, typename Arg>
    template <typename TaskFunc, typename... Args, typename TaskBuilder>
    auto Task::Builder<Invocable, Signature, Arg, false>::then(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        internal::ArityChecker<std::decay_t<ResultType>, typename TaskBuilder::PureArgs>::validate();
        constexpr bool HasToken = std::is_same_v<std::decay_t<typename TaskBuilder::FirstArg>, CancellationToken>;
        constexpr bool IsMember = core::traits::FunctionTraits<TaskFunc>::IsMemberFnPtr;

        auto invocable = internal::BuilderHelper<typename TaskBuilder::ResultType, ResultType, IsMember, HasToken>::createInvocable(
            executor, _task->getToken(), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();

        _last->setNext(std::move(invocable));
        return Builder<typename decltype(invocable)::element_type, TaskFunc, Arg>{std::move(_task), _last, last};
    }

    template <typename Invocable, typename Signature, typename Arg>
    auto Task::Builder<Invocable, Signature, Arg>::getFuture() -> std::future<Return>
    {
        auto task = static_cast<Invocable*>(this->_task->getLastTask());
        auto promised = task->toPromisedTask();
        auto future = promised->getFuture();

        if (this->_current == this->_last) {
            this->_task = std::move(promised);
            this->_current = this->_last = this->_task.get();
        }
        else {
            this->_current->setNext(std::move(promised));
        }

        return future;
    }

    template <typename Invocable, typename Signature, typename Arg>
    template <typename TaskFunc, typename... Args>
    auto Task::Builder<Invocable, Signature, Arg>::onException(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        auto task = static_cast<Invocable*>(this->_task->getLastTask());
        task->setupExceptionCallback(executor, std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        return TaskBuilder<TaskFunc>{std::move(this->_task), this->_current, this->_last};
    }

    template <typename Invocable, typename Signature, typename Arg>
    template <typename TaskFunc, typename... Args, typename>
    auto Task::Builder<Invocable, Signature, Arg>::onException(TaskFunc&& func, Args&&... args)
    {
        auto task = static_cast<Invocable*>(this->_task->getLastTask());
        task->setupExceptionCallback(this->_task->getExecutor(), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        return TaskBuilder<TaskFunc>{std::move(this->_task), this->_current, this->_last};
    }

    /// Task::PeriodicBuilder implementation
    /// ============================================================================================

    template <typename Invocable, typename Signature, typename Arg>
    template <typename TaskFunc, typename... Args, typename TaskBuilder>
    auto Task::PeriodicBuilder<Invocable, Signature, Arg>::then(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        internal::ArityChecker<
            std::decay_t<typename Builder<Invocable, Signature, Arg, false>::ResultType>, typename TaskBuilder::PureArgs>::validate();
        constexpr bool HasToken = std::is_same_v<std::decay_t<typename TaskBuilder::FirstArg>, CancellationToken>;
        constexpr bool IsMember = core::traits::FunctionTraits<TaskFunc>::IsMemberFnPtr;

        auto invocable = internal::BuilderHelper<
            typename TaskBuilder::ResultType, typename Builder<Invocable, Signature, Arg, false>::ResultType, IsMember,
            HasToken>::createInvocable(executor, this->_task->getToken(), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();

        this->_last->setNext(std::move(invocable));
        return PeriodicBuilder<typename decltype(invocable)::element_type, TaskFunc, Arg>{std::move(this->_task), this->_last, last};
    }

    template <typename Invocable, typename Signature, typename Arg>
    template <typename TaskFunc, typename... Args>
    auto Task::PeriodicBuilder<Invocable, Signature, Arg>::onException(TaskExecutor* executor, TaskFunc&& func, Args&&... args)
    {
        auto task = static_cast<Invocable*>(this->_task->getLastTask());
        task->setupExceptionCallback(executor, std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        return TaskBuilder<TaskFunc>{std::move(this->_task), this->_current, this->_last};
    }

    template <typename Invocable, typename Signature, typename Arg>
    template <typename TaskFunc, typename... Args, typename>
    auto Task::PeriodicBuilder<Invocable, Signature, Arg>::onException(TaskFunc&& func, Args&&... args)
    {
        auto task = static_cast<Invocable*>(this->_task->getLastTask());
        task->setupExceptionCallback(this->_task->getExecutor(), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        return TaskBuilder<TaskFunc>{std::move(this->_task), this->_current, this->_last};
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
        constexpr bool HasToken = std::is_same_v<std::decay_t<typename TaskBuilder::FirstArg>, CancellationToken>;
        constexpr bool IsMember = core::traits::FunctionTraits<TaskFunc>::IsMemberFnPtr;

        auto invocable = internal::BuilderHelper<typename TaskBuilder::ResultType, void, IsMember, HasToken>::createInvocable(
            executor, std::move(token), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();

        return Builder<typename decltype(invocable)::element_type, TaskFunc, void>{std::move(invocable), last, last};
    }

    template <class Rep, class Period, typename TaskFunc, typename... Args, typename TaskBuilder>
    auto Task::run(
        TaskExecutor* executor, CancellationToken token, std::chrono::duration<Rep, Period> delay, TaskFunc&& func, Args&&... args)
    {
        if (!delay.count()) {
            return Task::run(executor, token, std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        }

        constexpr bool HasToken = std::is_same_v<std::decay_t<typename TaskBuilder::FirstArg>, CancellationToken>;
        constexpr bool IsMember = core::traits::FunctionTraits<TaskFunc>::IsMemberFnPtr;

        auto invocable = internal::BuilderHelper<typename TaskBuilder::ResultType, void, IsMember, HasToken>::createInvocable(
            executor, std::move(token), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();
        auto scheduled = internal::TaskHelper::convertTask(std::move(invocable), delay);
        auto current = scheduled.get();

        return Builder<typename decltype(invocable)::element_type, TaskFunc, void>{std::move(scheduled), current, last};
    }

    template <class Rep1, class Period1, class Rep2, class Period2, typename TaskFunc, typename... Args, typename TaskBuilder>
    auto Task::run(
        TaskExecutor* executor,
        CancellationToken token,
        std::chrono::duration<Rep1, Period1> initialDelay,
        std::chrono::duration<Rep2, Period2> interval,
        TaskFunc&& func,
        Args&&... args)
    {
        constexpr bool HasToken = std::is_same_v<std::decay_t<typename TaskBuilder::FirstArg>, CancellationToken>;
        constexpr bool IsMember = core::traits::FunctionTraits<TaskFunc>::IsMemberFnPtr;

        auto invocable = internal::BuilderHelper<typename TaskBuilder::ResultType, void, IsMember, HasToken>::createInvocable(
            executor, std::move(token), std::forward<TaskFunc>(func), std::forward<Args>(args)...);
        auto last = invocable.get();
        auto scheduled = internal::TaskHelper::convertTask(std::move(invocable), initialDelay, interval);
        auto current = scheduled.get();

        return PeriodicBuilder<typename decltype(invocable)::element_type, TaskFunc, void>{std::move(scheduled), current, last};
    }

} // namespace vanilo::tasker

#endif // INC_CF1B33A15FDE47BDA967EACB24A90BED