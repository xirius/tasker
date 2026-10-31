#ifndef VANILO_TASKER_TASK_BUILDER_H
#define VANILO_TASKER_TASK_BUILDER_H

#include <vanilo/tasker/TaskExecutor.h>
#include <vanilo/tasker/TaskHandle.h>
#include <vanilo/tasker/detail/Node.h>

#include <chrono>
#include <memory>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

namespace vanilo::tasker {

    template <typename R>
    class TaskBuilder;

    template <typename R>
    class PeriodicTaskBuilder;

    /// Periodic intervals
    /// ============================================================================================

    /**
     * What a fixed-rate series does when the next run is due and the previous one hasn't finished (its whole chain).
     */
    enum class Overrun
    {
        Queue, // Queue the next run anyway: runs can overlap on a thread pool, and an executor that can't keep up
               // collects them in its queue, then runs them one after the other.
        Skip,  // Skip this period: runs never overlap or pile up. The next run comes at the next period that finds
               // the previous run finished, so a run that never finishes holds up the series.
    };

    /**
     * A periodic task runs at a fixed rate: its runs are due every interval from the first one, whatever each run
     * takes. The overrun says what happens when a run is due before the previous one has finished. Periods that the
     * timer itself missed are always skipped. A plain duration means the same as FixedRate with Overrun::Queue.
     */
    struct FixedRate
    {
        template <typename Rep, typename Period>
        FixedRate(
            const std::chrono::duration<Rep, Period> interval,
            const Overrun overrun = Overrun::Queue) // NOLINT(google-explicit-constructor)
            : interval{std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval)}, overrun{overrun}
        {
        }

        std::chrono::steady_clock::duration interval;
        Overrun overrun;
    };

    /**
     * A periodic task runs with a fixed delay between the end of a run (its whole chain) and the start of the
     * next one, so runs never overlap.
     */
    struct FixedDelay
    {
        template <typename Rep, typename Period>
        FixedDelay(const std::chrono::duration<Rep, Period> delay) // NOLINT(google-explicit-constructor)
            : delay{std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay)}
        {
        }

        std::chrono::steady_clock::duration delay;
    };

    namespace detail {

        template <typename T>
        struct IsDuration: std::false_type
        {
        };

        template <typename Rep, typename Period>
        struct IsDuration<std::chrono::duration<Rep, Period>>: std::true_type
        {
        };

        /**
         * What can be given as the interval of a periodic task: a duration, FixedRate or FixedDelay.
         */
        template <typename T>
        concept PeriodicInterval = IsDuration<T>::value || std::same_as<T, FixedRate> || std::same_as<T, FixedDelay>;

        enum class PeriodicMode
        {
            FixedRate,
            FixedRateSkip,
            FixedDelay,
        };

        /**
         * A chain under construction: its nodes, the user's stop token and its schedule.
         */
        struct ChainParts
        {
            std::unique_ptr<Node> head;
            Node* last{nullptr};
            std::stop_token token;
            std::chrono::steady_clock::duration delay{};  // before the (first) run
            std::chrono::steady_clock::duration period{}; // between runs of a periodic task
            PeriodicMode mode{PeriodicMode::FixedRate};

            void append(std::unique_ptr<Node> node) noexcept
            {
                Node* added = node.get();
                if (head) {
                    last->append(std::move(node));
                }
                else {
                    head = std::move(node);
                }
                last = added;
            }

            /**
             * Throws if the builder holding the parts was already used (moved from or submitted).
             */
            VANILO_EXPORT void checkUsable() const;
        };

        /**
         * Starts a one-shot chain: queues its first task, after the delay if there is one.
         * @throws std::bad_alloc The chain wasn't started.
         */
        VANILO_EXPORT void startChain(ChainParts parts, std::shared_ptr<ChainControl> control);

        /**
         * Starts a periodic chain.
         * @return The state of its handle.
         */
        VANILO_EXPORT std::shared_ptr<ChainState<void>> startSeries(ChainParts parts);

        template <typename R>
        ChainParts& partsOf(TaskBuilder<R>& builder) noexcept;

        template <typename Input, typename F, bool Copyable, typename... Args>
        std::unique_ptr<Node> makeTaskNode(TaskExecutor* executor, F&& function, Args&&... args)
        {
            using Created = TaskNode<Input, std::decay_t<F>, std::tuple<std::decay_t<Args>...>, Copyable>;
            return std::make_unique<Created>(coreOf(executor), std::forward<F>(function), std::forward<Args>(args)...);
        }

        template <typename F, bool Copyable>
        std::unique_ptr<Handler> makeObserver(TaskExecutor* executor, F&& handler)
        {
            return std::make_unique<ObserverHandler<std::decay_t<F>, Copyable>>(coreOf(executor), std::forward<F>(handler));
        }

        template <typename R, typename F, bool Copyable>
        std::unique_ptr<Handler> makeRecovery(TaskExecutor* executor, F&& handler)
        {
            return std::make_unique<RecoveryHandler<R, std::decay_t<F>, Copyable>>(coreOf(executor), std::forward<F>(handler));
        }

    } // namespace detail

    /// TaskBuilder
    /// ============================================================================================

    /**
     * Builds a chain of tasks, started by Task::run() and queued by submit(). Its methods are called on the
     * temporary (or on a std::move'd builder): each one returns the builder for the next step.
     *
     * Each task runs on the executor given for it and receives the previous task's result. When a task fails,
     * the rest of the chain doesn't run: the nearest onError handler from the failing task on receives the error,
     * the first recover handler from there on replaces the result and the chain goes on, and otherwise the chain
     * fails (see TaskHandle). A canceled chain fails with an OperationCanceledException.
     *
     * @tparam R The result of the last task.
     */
    template <typename R>
    class [[nodiscard]] TaskBuilder
    {
        template <typename>
        friend class TaskBuilder;

        template <typename T>
        friend detail::ChainParts& detail::partsOf(TaskBuilder<T>& builder) noexcept;

      public:
        using Result = R;

        explicit TaskBuilder(detail::ChainParts parts) noexcept: _parts{std::move(parts)}
        {
        }

        TaskBuilder(TaskBuilder&&) noexcept = default;
        TaskBuilder& operator=(TaskBuilder&&) noexcept = default;

        /**
         * Adds a task that runs on the executor after the previous one, receiving its result (unless it returns
         * void) after the bound arguments.
         */
        template <typename F, typename... Args>
        requires detail::TaskFunction<F, R, Args...>
        TaskBuilder<detail::TaskResult<F, R, Args...>> then(TaskExecutor* executor, F&& function, Args&&... args) &&
        {
            _parts.checkUsable();
            _parts.append(detail::makeTaskNode<R, F, false>(executor, std::forward<F>(function), std::forward<Args>(args)...));
            return TaskBuilder<detail::TaskResult<F, R, Args...>>{std::move(_parts)};
        }

        /**
         * Adds an error handler for the tasks before it (up to the previous recover handler). It is called with
         * the exception if it takes its type (std::exception_ptr takes any), and optionally the chain's
         * std::stop_token; it runs on the executor of the task before it. The chain still fails.
         */
        template <typename F>
        requires detail::ErrorHandlerFunction<F>
        TaskBuilder onError(F&& handler) &&
        {
            return std::move(*this).onError(nullptr, std::forward<F>(handler));
        }

        /**
         * Like onError(handler), the handler running on the given executor.
         */
        template <typename F>
        requires detail::ErrorHandlerFunction<F>
        TaskBuilder onError(TaskExecutor* executor, F&& handler) &&
        {
            _parts.checkUsable();
            _parts.last->addHandler(detail::makeObserver<F, false>(executor, std::forward<F>(handler)));
            return std::move(*this);
        }

        /**
         * Adds a recovery for the tasks before it: called like an onError handler, it returns the result that
         * replaces the one of the task before it, and the chain goes on. If it throws, that error is delivered
         * instead.
         */
        template <typename F>
        requires detail::RecoveryFunction<F, R>
        TaskBuilder recover(F&& handler) &&
        {
            return std::move(*this).recover(nullptr, std::forward<F>(handler));
        }

        /**
         * Like recover(handler), the handler running on the given executor.
         */
        template <typename F>
        requires detail::RecoveryFunction<F, R>
        TaskBuilder recover(TaskExecutor* executor, F&& handler) &&
        {
            _parts.checkUsable();
            _parts.last->addHandler(detail::makeRecovery<R, F, false>(executor, std::forward<F>(handler)));
            return std::move(*this);
        }

        /**
         * Queues the chain.
         * @return The handle of the chain; it may be dropped.
         */
        TaskHandle<R> submit() &&
        {
            _parts.checkUsable();
            auto state = std::make_shared<detail::ChainState<R>>(std::move(_parts.token));
            detail::startChain(std::move(_parts), state);
            return TaskHandle<R>{std::move(state)};
        }

      private:
        detail::ChainParts _parts;
    };

    /// PeriodicTaskBuilder
    /// ============================================================================================

    /**
     * Builds a periodic chain, started by Task::run() with an interval: like TaskBuilder, but each run is a copy
     * of the chain, so its functions must be copyable. Errors of a run that no onError handler receives are
     * reported to the unhandled-exception handler, and the series goes on.
     *
     * @tparam R The result of the last task.
     */
    template <typename R>
    class [[nodiscard]] PeriodicTaskBuilder
    {
        template <typename>
        friend class PeriodicTaskBuilder;

      public:
        using Result = R;

        explicit PeriodicTaskBuilder(detail::ChainParts parts) noexcept: _parts{std::move(parts)}
        {
        }

        PeriodicTaskBuilder(PeriodicTaskBuilder&&) noexcept = default;
        PeriodicTaskBuilder& operator=(PeriodicTaskBuilder&&) noexcept = default;

        /**
         * See TaskBuilder::then().
         */
        template <typename F, typename... Args>
        requires detail::PeriodicTaskFunction<F, R, Args...>
        PeriodicTaskBuilder<detail::TaskResult<F, R, Args...>> then(TaskExecutor* executor, F&& function, Args&&... args) &&
        {
            _parts.checkUsable();
            _parts.append(detail::makeTaskNode<R, F, true>(executor, std::forward<F>(function), std::forward<Args>(args)...));
            return PeriodicTaskBuilder<detail::TaskResult<F, R, Args...>>{std::move(_parts)};
        }

        /**
         * See TaskBuilder::onError().
         */
        template <typename F>
        requires detail::ErrorHandlerFunction<F> && std::copy_constructible<std::decay_t<F>>
        PeriodicTaskBuilder onError(F&& handler) &&
        {
            return std::move(*this).onError(nullptr, std::forward<F>(handler));
        }

        /**
         * See TaskBuilder::onError().
         */
        template <typename F>
        requires detail::ErrorHandlerFunction<F> && std::copy_constructible<std::decay_t<F>>
        PeriodicTaskBuilder onError(TaskExecutor* executor, F&& handler) &&
        {
            _parts.checkUsable();
            _parts.last->addHandler(detail::makeObserver<F, true>(executor, std::forward<F>(handler)));
            return std::move(*this);
        }

        /**
         * See TaskBuilder::recover().
         */
        template <typename F>
        requires detail::RecoveryFunction<F, R> && std::copy_constructible<std::decay_t<F>>
        PeriodicTaskBuilder recover(F&& handler) &&
        {
            return std::move(*this).recover(nullptr, std::forward<F>(handler));
        }

        /**
         * See TaskBuilder::recover().
         */
        template <typename F>
        requires detail::RecoveryFunction<F, R> && std::copy_constructible<std::decay_t<F>>
        PeriodicTaskBuilder recover(TaskExecutor* executor, F&& handler) &&
        {
            _parts.checkUsable();
            _parts.last->addHandler(detail::makeRecovery<R, F, true>(executor, std::forward<F>(handler)));
            return std::move(*this);
        }

        /**
         * Starts the series.
         * @return The handle of the series: cancel() ends it, and it is ready once the series has ended.
         */
        TaskHandle<void> submit() &&
        {
            _parts.checkUsable();
            return TaskHandle<void>{detail::startSeries(std::move(_parts))};
        }

      private:
        detail::ChainParts _parts;
    };

    namespace detail {
        template <typename R>
        ChainParts& partsOf(TaskBuilder<R>& builder) noexcept
        {
            return builder._parts;
        }
    } // namespace detail

} // namespace vanilo::tasker

#endif // VANILO_TASKER_TASK_BUILDER_H
