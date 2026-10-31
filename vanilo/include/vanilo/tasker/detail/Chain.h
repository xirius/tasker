#ifndef VANILO_TASKER_DETAIL_CHAIN_H
#define VANILO_TASKER_DETAIL_CHAIN_H

#include <vanilo/Export.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>

namespace vanilo::tasker::detail {

    /**
     * The OperationCanceledException that canceled chains receive. It is created once, so canceling a chain
     * doesn't throw.
     */
    VANILO_EXPORT const std::exception_ptr& canceledException() noexcept;

    /**
     * @return True if the error is an OperationCanceledException.
     */
    VANILO_EXPORT bool isCancellation(const std::exception_ptr& error) noexcept;

    /**
     * Passes an exception that escaped a task to the unhandled-exception handler. Cancellations are ignored.
     */
    VANILO_EXPORT void reportUnhandledException(const std::exception_ptr& exception) noexcept;

    /// ChainControl
    /// ============================================================================================

    /**
     * The shared state of a running chain: its stop source, which the chain's tasks check and receive as their
     * std::stop_token, and where the chain's outcome goes. The node that runs (or waits to run) holds it, and it
     * moves from node to node.
     */
    class VANILO_EXPORT ChainControl
    {
      public:
        /**
         * @param first, second Tokens that stop the chain too (a user's token, the stop of an enclosing whenAll).
         */
        explicit ChainControl(std::stop_token first = {}, std::stop_token second = {});
        virtual ~ChainControl();

        ChainControl(const ChainControl&) = delete;
        ChainControl& operator=(const ChainControl&) = delete;

        [[nodiscard]] bool stopRequested() const noexcept
        {
            return _source.stop_requested();
        }

        [[nodiscard]] std::stop_token token() const noexcept
        {
            return _source.get_token();
        }

        void requestStop() noexcept
        {
            _source.request_stop();
        }

        /**
         * The chain finished. value points to its result, which may be moved from (nullptr for void).
         */
        virtual void complete(void* value) noexcept = 0;

        /**
         * The chain failed; handled tells whether an onError handler received the error.
         */
        virtual void fail(const std::exception_ptr& error, bool handled) noexcept = 0;

      private:
        struct StopLink
        {
            std::stop_source* source;

            void operator()() const noexcept
            {
                source->request_stop();
            }
        };

        // The links register nothing for a token without a stop state; they are declared after the source,
        // which a link may stop as soon as it is constructed.
        std::stop_source _source;
        std::stop_callback<StopLink> _firstLink;
        std::stop_callback<StopLink> _secondLink;
    };

    /// Completion
    /// ============================================================================================

    /**
     * The completion state behind a TaskHandle: whether the outcome is known, waiting for it, and reporting an
     * error that nobody handled or looked at.
     *
     * An error that no onError handler received is reported to the unhandled-exception handler once the handle
     * is gone without get() having been called (cancellations are never reported).
     */
    class VANILO_EXPORT Completion
    {
      public:
        Completion() = default;
        Completion(const Completion&) = delete;
        Completion& operator=(const Completion&) = delete;

        [[nodiscard]] bool isReady() const noexcept
        {
            return _status.load(std::memory_order_acquire) != Pending;
        }

        void wait() const noexcept;

        /**
         * @return True if the outcome is known, false if the deadline passed first.
         */
        bool waitUntil(std::chrono::steady_clock::time_point deadline) const;

        /**
         * The handle let go of the state. observed tells whether it saw the outcome (get() was called).
         */
        void releaseHandle(bool observed) noexcept;

      protected:
        ~Completion() = default;

        void publishValue() noexcept;
        void publishError(const std::exception_ptr& error, bool reportIfUnobserved) noexcept;

        [[nodiscard]] const std::exception_ptr& error() const noexcept
        {
            return _error;
        }

      private:
        static constexpr std::uint32_t Pending = 0;
        static constexpr std::uint32_t Done = 1;

        static constexpr std::uint32_t ErrorUnhandled = 1;
        static constexpr std::uint32_t HandleReleased = 2;
        static constexpr std::uint32_t Observed = 4;

        void publish() noexcept;

        std::atomic<std::uint32_t> _status{Pending};
        std::atomic<std::uint32_t> _flags{0};
        mutable std::atomic<int> _timedWaiters{0};
        mutable std::mutex _mutex;
        mutable std::condition_variable _condition;
        std::exception_ptr _error;
    };

    /// ChainState
    /// ============================================================================================

    /**
     * The state shared by a chain and its TaskHandle: the chain's control and its result.
     */
    template <typename T>
    class ChainState: public ChainControl, public Completion
    {
        using Stored = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

      public:
        using ChainControl::ChainControl;

        void complete(void* value) noexcept override
        {
            try {
                if constexpr (std::is_void_v<T>) {
                    _value.emplace();
                }
                else {
                    _value.emplace(std::move(*static_cast<T*>(value)));
                }
                publishValue();
            }
            catch (...) {
                publishError(std::current_exception(), true);
            }
        }

        void fail(const std::exception_ptr& error, const bool handled) noexcept override
        {
            publishError(error, !handled && !isCancellation(error));
        }

        /**
         * The outcome, once ready: returns the result or rethrows the error.
         */
        T take()
        {
            if (error()) {
                std::rethrow_exception(error());
            }
            if constexpr (!std::is_void_v<T>) {
                return std::move(*_value);
            }
        }

      private:
        std::optional<Stored> _value;
    };

} // namespace vanilo::tasker::detail

#endif // VANILO_TASKER_DETAIL_CHAIN_H
