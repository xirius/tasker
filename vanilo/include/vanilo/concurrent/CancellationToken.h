#pragma once
#include <vanilo/Export.h>

#include <functional>
#include <memory>

namespace vanilo::concurrent {
    /**
     * This exception occurs when you're waiting for a result, then a cancellation is notified.
     */
    class VANILO_EXPORT OperationCanceledException final: public std::exception
    {
      public:
        [[nodiscard]] const char* what() const noexcept override
        {
            return "The operation was canceled.";
        }
    };

    /**
     * Object used to propagate notification that operations should be canceled.
     */
    class VANILO_EXPORT CancellationToken
    {
      public:
        class Subscription;

        static CancellationToken none();

        CancellationToken();

        friend bool operator==(const CancellationToken& lhs, const CancellationToken& rhs) noexcept;
        friend bool operator!=(const CancellationToken& lhs, const CancellationToken& rhs) noexcept;

        /**
         * Communicates a request for cancellation.
         */
        void cancel() const;

        /**
         * Gets whether cancellation has been requested.
         * @return True if cancellation has been requested, false otherwise.
         */
        [[nodiscard]] bool isCancellationRequested() const noexcept;

        /**
         * Registers a delegate that will be called when this CancellationToken is canceled.
         * @param callback The callback to be executed when the CancellationToken is canceled.
         * @return The subscription instance that can be used to unregister the callback.
         */
        Subscription subscribe(std::function<void()> callback) const;

        /**
         * Throws a CanceledException if this token has had cancellation requested.
         */
        void throwIfCancellationRequested() const;

      private:
        struct Impl;
        std::shared_ptr<Impl> _impl;
    };

    /**
     * Represents a callback delegate that has been registered with a CancellationToken.
     */
    class VANILO_EXPORT CancellationToken::Subscription
    {
        friend CancellationToken;

      public:
        ~Subscription();

        /**
         * Unregisters the target callback from the associated CancellationToken.
         */
        void unsubscribe() const;

      private:
        Subscription();

        struct Impl;
        std::shared_ptr<Impl> _impl;
    };

} // namespace vanilo::concurrent