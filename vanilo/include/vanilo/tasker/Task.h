#ifndef VANILO_TASKER_TASK_H
#define VANILO_TASKER_TASK_H

#include <vanilo/tasker/TaskBuilder.h>

#include <chrono>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

namespace vanilo::tasker {

    namespace detail {

        /**
         * Queues a job on the executor, or drops it if the executor doesn't take it.
         */
        VANILO_EXPORT void post(TaskExecutor* executor, Job job);

        template <bool HasToken>
        struct PostToken
        {
            explicit PostToken(std::stop_token) noexcept
            {
            }

            [[nodiscard]] static bool stopRequested() noexcept
            {
                return false;
            }

            [[nodiscard]] static std::stop_token get() noexcept
            {
                return {};
            }
        };

        template <>
        struct PostToken<true>
        {
            explicit PostToken(std::stop_token token) noexcept: token{std::move(token)}
            {
            }

            [[nodiscard]] bool stopRequested() const noexcept
            {
                return token.stop_requested();
            }

            [[nodiscard]] const std::stop_token& get() const noexcept
            {
                return token;
            }

            std::stop_token token;
        };

        /**
         * The job of Task::post(): runs the function unless a stop was requested; dropped when canceled.
         */
        template <typename F, typename Bound, bool HasToken>
        struct PostJob
        {
            template <typename Function, typename... Args>
            PostJob(std::stop_token stopToken, Function&& function, Args&&... args)
                : function(std::forward<Function>(function)), bound(std::forward<Args>(args)...), token{std::move(stopToken)}
            {
            }

            void run()
            {
                if (token.stopRequested()) {
                    return;
                }
                const auto& stopToken = token.get();
                Invocation<F, Bound, void>::call(function, bound, stopToken, nullptr);
            }

            void cancel() noexcept
            {
                // Dropped: there is no one to tell
            }

            F function;
            [[no_unique_address]] Bound bound;
            [[no_unique_address]] PostToken<HasToken> token;
        };

        template <bool HasToken, typename F, typename... Args>
        void postFunction(TaskExecutor* executor, std::stop_token token, F&& function, Args&&... args)
        {
            using Target = PostJob<std::decay_t<F>, std::tuple<std::decay_t<Args>...>, HasToken>;
            post(executor, Job::make<Target>(std::move(token), std::forward<F>(function), std::forward<Args>(args)...));
        }

        template <typename Rep, typename Period>
        std::chrono::steady_clock::duration toSteadyDuration(const std::chrono::duration<Rep, Period> duration)
        {
            return std::chrono::ceil<std::chrono::steady_clock::duration>(duration);
        }

        template <typename Interval>
        ChainParts periodicParts(std::stop_token token, const std::chrono::steady_clock::duration initialDelay, const Interval& interval)
        {
            ChainParts parts;
            parts.token = std::move(token);
            parts.delay = initialDelay;

            if constexpr (std::is_same_v<Interval, FixedDelay>) {
                parts.mode = PeriodicMode::FixedDelay;
                parts.period = interval.delay;
            }
            else if constexpr (std::is_same_v<Interval, FixedRate>) {
                parts.period = interval.interval;
            }
            else {
                parts.period = toSteadyDuration(interval);
            }

            if (parts.period <= std::chrono::steady_clock::duration::zero()) {
                throw std::invalid_argument{"The interval of a periodic task must be positive."};
            }
            return parts;
        }

    } // namespace detail

    /// Task
    /// ============================================================================================

    /**
     * Starts tasks: Task::run() builds a chain that submit() queues, Task::post() queues a single function.
     *
     * A Task object is also the low-level unit that TaskExecutor::submit() queues: implement run(), and cancel(),
     * which is called instead when the executor shuts down before running it.
     *
     * Functions are copied (or moved) into the task with their bound arguments; use std::ref for an argument
     * the task should change in place. A function whose first parameter is a std::stop_token receives the
     * chain's token there (after the object, for a member function).
     */
    class VANILO_EXPORT Task
    {
      public:
        virtual ~Task() = default;

        /**
         * Runs the task. An exception that escapes it is reported to the unhandled-exception handler.
         */
        virtual void run() = 0;

        /**
         * Called instead of run() when the executor shuts down before running the task.
         */
        virtual void cancel() noexcept = 0;

        /// Chains
        /// ========================================================================================

        /**
         * Starts a chain with a task running on the executor.
         */
        template <typename F, typename... Args>
        requires detail::TaskFunction<F, void, Args...>
        static TaskBuilder<detail::TaskResult<F, void, Args...>> run(TaskExecutor* executor, F&& function, Args&&... args)
        {
            return start(executor, {}, {}, std::forward<F>(function), std::forward<Args>(args)...);
        }

        /**
         * Starts a chain that a stop request through the token cancels.
         */
        template <typename F, typename... Args>
        requires detail::TaskFunction<F, void, Args...>
        static TaskBuilder<detail::TaskResult<F, void, Args...>> run(
            TaskExecutor* executor, std::stop_token token, F&& function, Args&&... args)
        {
            return start(executor, std::move(token), {}, std::forward<F>(function), std::forward<Args>(args)...);
        }

        /**
         * Starts a chain whose first task is queued after the delay.
         */
        template <typename Rep, typename Period, typename F, typename... Args>
        requires detail::TaskFunction<F, void, Args...>
        static TaskBuilder<detail::TaskResult<F, void, Args...>> run(
            TaskExecutor* executor, std::chrono::duration<Rep, Period> delay, F&& function, Args&&... args)
        {
            return start(executor, {}, detail::toSteadyDuration(delay), std::forward<F>(function), std::forward<Args>(args)...);
        }

        /**
         * Starts a chain whose first task is queued after the delay, canceled by a stop request through the token.
         */
        template <typename Rep, typename Period, typename F, typename... Args>
        requires detail::TaskFunction<F, void, Args...>
        static TaskBuilder<detail::TaskResult<F, void, Args...>> run(
            TaskExecutor* executor, std::stop_token token, std::chrono::duration<Rep, Period> delay, F&& function, Args&&... args)
        {
            return start(
                executor, std::move(token), detail::toSteadyDuration(delay), std::forward<F>(function), std::forward<Args>(args)...);
        }

        /// Periodic chains
        /// ========================================================================================

        /**
         * Starts a periodic chain: first after initialDelay, then at the interval, which is a duration or
         * FixedRate (fixed rate) or FixedDelay (a delay after each run). It runs until it is canceled through its
         * handle, or its executor shuts down.
         * @throws std::invalid_argument if the interval isn't positive.
         */
        template <typename Rep, typename Period, detail::PeriodicInterval Interval, typename F, typename... Args>
        requires detail::PeriodicTaskFunction<F, void, Args...>
        static PeriodicTaskBuilder<detail::TaskResult<F, void, Args...>> run(
            TaskExecutor* executor, std::chrono::duration<Rep, Period> initialDelay, Interval interval, F&& function, Args&&... args)
        {
            return startPeriodic(executor, {}, initialDelay, interval, std::forward<F>(function), std::forward<Args>(args)...);
        }

        /**
         * Like the periodic run() above, also ended by a stop request through the token.
         */
        template <typename Rep, typename Period, detail::PeriodicInterval Interval, typename F, typename... Args>
        requires detail::PeriodicTaskFunction<F, void, Args...>
        static PeriodicTaskBuilder<detail::TaskResult<F, void, Args...>> run(
            TaskExecutor* executor,
            std::stop_token token,
            std::chrono::duration<Rep, Period> initialDelay,
            Interval interval,
            F&& function,
            Args&&... args)
        {
            return startPeriodic(
                executor, std::move(token), initialDelay, interval, std::forward<F>(function), std::forward<Args>(args)...);
        }

        /// Fire and forget
        /// ========================================================================================

        /**
         * Queues the function on the executor, without a chain or a handle: the cheapest way to run something.
         * An exception that escapes it is reported to the unhandled-exception handler. If the executor is shutting
         * down, the function is dropped.
         */
        template <typename F, typename... Args>
        requires detail::TaskFunction<F, void, Args...>
        static void post(TaskExecutor* executor, F&& function, Args&&... args)
        {
            detail::postFunction<false>(executor, {}, std::forward<F>(function), std::forward<Args>(args)...);
        }

        /**
         * Like post(executor, function, args...); the function doesn't run if a stop was requested through the
         * token by the time it would.
         */
        template <typename F, typename... Args>
        requires detail::TaskFunction<F, void, Args...>
        static void post(TaskExecutor* executor, std::stop_token token, F&& function, Args&&... args)
        {
            detail::postFunction<true>(executor, std::move(token), std::forward<F>(function), std::forward<Args>(args)...);
        }

      private:
        template <typename F, typename... Args>
        static TaskBuilder<detail::TaskResult<F, void, Args...>> start(
            TaskExecutor* executor, std::stop_token token, const std::chrono::steady_clock::duration delay, F&& function, Args&&... args)
        {
            detail::ChainParts parts;
            parts.token = std::move(token);
            parts.delay = delay;
            parts.append(detail::makeTaskNode<void, F, false>(executor, std::forward<F>(function), std::forward<Args>(args)...));
            return TaskBuilder<detail::TaskResult<F, void, Args...>>{std::move(parts)};
        }

        template <typename Rep, typename Period, typename Interval, typename F, typename... Args>
        static PeriodicTaskBuilder<detail::TaskResult<F, void, Args...>> startPeriodic(
            TaskExecutor* executor,
            std::stop_token token,
            const std::chrono::duration<Rep, Period> initialDelay,
            const Interval& interval,
            F&& function,
            Args&&... args)
        {
            auto parts = detail::periodicParts(std::move(token), detail::toSteadyDuration(initialDelay), interval);
            parts.append(detail::makeTaskNode<void, F, true>(executor, std::forward<F>(function), std::forward<Args>(args)...));
            return PeriodicTaskBuilder<detail::TaskResult<F, void, Args...>>{std::move(parts)};
        }
    };

} // namespace vanilo::tasker

#endif // VANILO_TASKER_TASK_H
