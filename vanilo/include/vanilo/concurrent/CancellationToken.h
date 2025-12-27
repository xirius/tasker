#pragma once
#include <vanilo/Export.h>

#include <functional>
#include <memory>
#include <stdexcept>

namespace vanilo::concurrent {
    /**
     * This exception occurs when you're waiting for a result, then a cancellation is notified.
     */
    class VANILO_EXPORT OperationCanceledException final: public std::runtime_error
    {
      public:
        OperationCanceledException(): std::runtime_error("The operation was canceled.")
        {
        }
    };

    /**
     * Object used to propagate notification that operations should be canceled.
     */
    class VANILO_EXPORT CancellationToken
    {
        friend class CancellationTokenSource;

      public:
        class Subscription;

        /**
         * @return A token that never cancels.
         */
        static CancellationToken none();

        CancellationToken() noexcept = default;

        friend bool operator==(const CancellationToken& a, const CancellationToken& b) noexcept;
        friend bool operator!=(const CancellationToken& a, const CancellationToken& b) noexcept;

        /**
         * Gets whether cancellation has been requested.
         * @return True if cancellation has been requested, false otherwise.
         */
        [[nodiscard]] bool isCancellationRequested() const noexcept;

        /**
         * Throws a CanceledException if this token has had cancellation requested.
         */
        void throwIfCancellationRequested() const;

        /**
         * Registers a delegate that will be called when this CancellationToken is canceled.
         * If already canceled, the callback is invoked immediately (synchronously).
         * @param callback The callback to be executed when the CancellationToken is canceled.
         * @return The subscription instance that can be used to unregister the callback.
         */
        Subscription subscribe(std::function<void()> callback) const;

      private:
        struct Impl;
        explicit CancellationToken(std::shared_ptr<Impl> impl) noexcept;
        std::shared_ptr<Impl> _impl;
    };

    /**
     * Represents a callback delegate that has been registered with a CancellationToken.
     */
    class VANILO_EXPORT CancellationToken::Subscription
    {
        friend CancellationToken;

      public:
        Subscription(const Subscription&) = delete;
        Subscription(Subscription&&) noexcept = default;
        ~Subscription();

        Subscription& operator=(const Subscription&) = delete;
        Subscription& operator=(Subscription&&) noexcept = default;

        /**
         * Unregisters the target callback from the associated CancellationToken.
         */
        void unsubscribe() noexcept;

      private:
        struct Impl;
        Subscription() noexcept = default;
        explicit Subscription(std::shared_ptr<Impl> impl) noexcept;

        std::shared_ptr<Impl> _impl;
    };

    /**
     * Provides a mechanism for signaling cancellation to one or more tasks or operations.
     *
     * A `CancellationTokenSource` creates a `CancellationToken` that can be used to observe
     * or respond to cancellation requests. The source and token work together to provide
     * cooperative cancellation.
     */
    class CancellationTokenSource
    {
      public:
        CancellationTokenSource();

        /**
         * Requests cancellation. Idempotent.
         */
        void cancel() const noexcept;

        [[nodiscard]] bool isCancellationRequested() const noexcept;

        [[nodiscard]] CancellationToken token() const noexcept;

      private:
        std::shared_ptr<CancellationToken::Impl> _impl;
    };

} // namespace vanilo::concurrent