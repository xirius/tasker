#ifndef VANILO_TASKER_TASK_HANDLE_H
#define VANILO_TASKER_TASK_HANDLE_H

#include <vanilo/tasker/detail/Chain.h>

#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace vanilo::tasker {

    /**
     * The handle of a submitted chain (see TaskBuilder::submit): cancels it, waits for it, and gets its result.
     *
     * Like std::future, it is move-only and get() can be called once. Dropping the handle doesn't cancel the
     * chain. If the chain fails with an error that no onError handler received, and get() is never called,
     * the error is reported to the unhandled-exception handler when the handle is gone.
     *
     * The handle of a periodic task is ready once the series has ended (it was canceled), and get() then throws
     * OperationCanceledException.
     */
    template <typename T>
    class TaskHandle
    {
      public:
        using State = detail::ChainState<T>;

        TaskHandle() noexcept = default;

        explicit TaskHandle(std::shared_ptr<State> state) noexcept: _state{std::move(state)}
        {
        }

        TaskHandle(TaskHandle&& other) noexcept = default;

        TaskHandle& operator=(TaskHandle&& other) noexcept
        {
            if (this != &other) {
                release(false);
                _state = std::move(other._state);
            }
            return *this;
        }

        ~TaskHandle()
        {
            release(false);
        }

        TaskHandle(const TaskHandle&) = delete;
        TaskHandle& operator=(const TaskHandle&) = delete;

        /**
         * @return True if the handle refers to a chain (it is not default-constructed, moved from or used by get()).
         */
        [[nodiscard]] bool valid() const noexcept
        {
            return _state != nullptr;
        }

        /**
         * Requests the cancellation of the chain: its tasks that haven't started don't run, and the ones that
         * take a std::stop_token see the stop request. Does nothing on an empty handle.
         */
        void cancel() const noexcept
        {
            if (_state) {
                _state->requestStop();
            }
        }

        /**
         * @return True if the chain has finished (with a result or an error).
         */
        [[nodiscard]] bool isReady() const noexcept
        {
            return _state && _state->isReady();
        }

        /**
         * Waits until the chain has finished.
         */
        void wait() const
        {
            state().wait();
        }

        /**
         * Waits until the chain has finished or the timeout has passed.
         * @return True if the chain has finished.
         */
        template <typename Rep, typename Period>
        bool waitFor(const std::chrono::duration<Rep, Period> timeout) const
        {
            return state().waitUntil(std::chrono::steady_clock::now() + std::chrono::ceil<std::chrono::steady_clock::duration>(timeout));
        }

        /**
         * Waits until the chain has finished or the deadline has passed.
         * @return True if the chain has finished.
         */
        template <typename Clock, typename Duration>
        bool waitUntil(const std::chrono::time_point<Clock, Duration> deadline) const
        {
            if constexpr (std::is_same_v<Clock, std::chrono::steady_clock>) {
                return state().waitUntil(std::chrono::time_point_cast<std::chrono::steady_clock::duration>(deadline));
            }
            else {
                return waitFor(deadline - Clock::now());
            }
        }

        /**
         * Waits until the chain has finished, then returns its result or rethrows its error. The handle is
         * empty afterwards.
         */
        T get()
        {
            state().wait();
            auto state = std::move(_state);
            state->releaseHandle(true);
            return state->take();
        }

      private:
        State& state() const
        {
            if (!_state) {
                throw std::future_error{std::future_errc::no_state};
            }
            return *_state;
        }

        void release(const bool observed) noexcept
        {
            if (_state) {
                _state->releaseHandle(observed);
                _state.reset();
            }
        }

        std::shared_ptr<State> _state;
    };

} // namespace vanilo::tasker

#endif // VANILO_TASKER_TASK_HANDLE_H
