#pragma once
#include <vanilo/Export.h>

#include <functional>
#include <memory>
#include <stdexcept>

namespace vanilo::concurrent {

    /**
     * Exception indicating that an operation was canceled.
     *
     * It is thrown by `CancellationToken::throwIfCancellationRequested()` and can be used by
     * code that cooperatively observes cancellation to abort work.
     */
    class VANILO_EXPORT OperationCanceledException final: public std::runtime_error
    {
      public:
        OperationCanceledException(): std::runtime_error("The operation was canceled.")
        {
        }
    };

    /**
     * Read-only handle used to observe cancellation requests.
     *
     * A `CancellationToken` is obtained from a `CancellationTokenSource` and allows consumers to:
     *  - Check whether cancellation has been requested via `isCancellationRequested()`.
     *  - Throw `OperationCanceledException` via `throwIfCancellationRequested()`.
     *  - Register callbacks via `subscribe()` that will be invoked when cancellation is requested.
     *
     * Notes:
     *  - A default-constructed token, or the one returned by `none()`, is never cancelable and
     *    will never transition to the "canceled" state.
     *  - Tokens are inexpensive to copy and are thread-safe to use concurrently.
     *  - Callbacks registered with `subscribe()` are invoked synchronously on the thread that
     *    calls `CancellationTokenSource::cancel()`, or immediately from `subscribe()` if the
     *    token was already canceled. Callbacks are invoked at most once.
     *  - Callback invocation order is not guaranteed. Avoid long-running work or blocking in
     *    callbacks to prevent delaying other listeners.
     */
    class VANILO_EXPORT CancellationToken
    {
        friend class CancellationTokenSource;

      public:
        class Subscription;

        /**
         * Creates a token that never cancels.
         *
         * @return A token that is permanently in the non-canceled state. `subscribe()` on this
         *         token is a no-op and returns an empty `Subscription`.
         */
        static CancellationToken none();

        CancellationToken() noexcept = default;

        friend bool operator==(const CancellationToken& a, const CancellationToken& b) noexcept;
        friend bool operator!=(const CancellationToken& a, const CancellationToken& b) noexcept;

        /**
         * Gets whether cancellation has been requested.
         *
         * Memory ordering: observes the cancel state with acquire semantics, which pairs with the
         * release in `CancellationTokenSource::cancel()`.
         *
         * Thread-safety: safe to call concurrently from multiple threads.
         *
         * @return True if cancellation has been requested, false otherwise.
         */
        [[nodiscard]] bool isCancellationRequested() const noexcept;

        /**
         * Throws an `OperationCanceledException` if this token has had cancellation requested.
         *
         * This is a convenience wrapper around `isCancellationRequested()` that simplifies
         * cooperative cancellation in long-running loops and operations.
         */
        void throwIfCancellationRequested() const;

        /**
         * Registers a callback that will be called when this `CancellationToken` is canceled.
         *
         * Semantics:
         *  - If the token is already canceled at registration time, the callback is invoked
         *    synchronously before this function returns.
         *  - Otherwise, the callback will be invoked synchronously on the thread executing
         *    `CancellationTokenSource::cancel()` at most once.
         *  - The callback must be noexcept with respect to the caller: any exception thrown by
         *    the callback is caught and logged, and then discarded.
         *  - Order of invocation among multiple callbacks is unspecified.
         *
         * Lifetime:
         *  - The returned `Subscription` is move-only and represents the registration. Destroying
         *    it or calling `unsubscribe()` unregisters the callback if it has not run yet.
         *  - It is safe to call `unsubscribe()` concurrently with cancellation; the callback will
         *    be invoked at most once or suppressed if successfully unregistered beforehand.
         *
         * @param callback The function to be executed when the token is canceled.
         * @return A `Subscription` that can be used to unregister the callback.
         */
        Subscription subscribe(std::function<void()> callback) const;

      private:
        struct Impl;
        explicit CancellationToken(std::shared_ptr<Impl> impl) noexcept;
        std::shared_ptr<Impl> _impl;
    };

    /**
     * Represents a registration of a callback with a `CancellationToken`.
     *
     * The type is move-only. Destroying a `Subscription` or calling `unsubscribe()` will attempt
     * to remove the registered callback if it has not run yet. These operations are idempotent
     * and thread-safe.
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
         * Unregisters the target callback from the associated `CancellationToken`.
         *
         * Idempotent and noexcept. Safe to call concurrently with cancellation; in that case the
         * callback may already be running or have run, and the call becomes a no-op.
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
     * A `CancellationTokenSource` owns the mutable cancel state and can create `CancellationToken`
     * instances that observe that state. Multiple tokens copied from the same source observe the
     * same cancel request.
     *
     * Thread-safety: all member functions are safe to call concurrently. `cancel()` is idempotent.
     */
    class CancellationTokenSource
    {
      public:
        CancellationTokenSource();

        /**
         * Requests cancellation. Idempotent.
         *
         * Invokes all registered callbacks synchronously on the calling thread. Callbacks are
         * invoked without holding internal locks; nevertheless, keep callbacks short to avoid
         * delaying other listeners.
         */
        void cancel() const noexcept;

        [[nodiscard]] bool isCancellationRequested() const noexcept;

        [[nodiscard]] CancellationToken token() const noexcept;

      private:
        std::shared_ptr<CancellationToken::Impl> _impl;
    };

} // namespace vanilo::concurrent