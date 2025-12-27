#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/core/Tracer.h>

#include <atomic>
#include <mutex>
#include <vector>

using namespace vanilo::concurrent;

/// CancellationToken::Subscription::Impl
/// ========================================================================

struct CancellationToken::Subscription::Impl
{
    explicit Impl(std::function<void()> callback): _callback{std::move(callback)}
    {
    }

    void unsubscribe()
    {
        _callback = nullptr;
    }

    void notify() const
    {
        if (_callback) {
            try {
                _callback();
            }
            catch (const std::exception& ex) {
                TRACE("An unhandled exception occurred during cancellation notification. Message: %s", ex.what());
            }
            catch (...) {
                TRACE("An unhandled exception occurred during cancellation notification!");
            }
        }
    }

    std::function<void()> _callback;
};

/// CancellationToken::Impl
/// ========================================================================

struct CancellationToken::Impl
{
    void cancel()
    {
        std::vector<std::weak_ptr<Subscription::Impl>> snapshot;

        {
            // Mutually exclusive with subscribe method
            std::lock_guard lock{mutex};
            if (canceled.exchange(true, std::memory_order_release)) {
                return; // already canceled
            }

            snapshot = subscriptions; // copy
        }

        // Callbacks have to be called without acquired mutex
        for (const auto& subscription : snapshot) {
            if (const auto callback = subscription.lock()) {
                callback->notify();
            }
        }
    }

    /**
     * Registers the callback which is called when the cancellation has been requested.
     * @return False if the cancellation has been requested and the registration cannot be done, true otherwise.
     */
    bool subscribe(std::weak_ptr<Subscription::Impl> subscription)
    {
        // No additional subscription can be made if the cancellation has been requested
        std::lock_guard lock{mutex};
        const size_t collectionSize = subscriptions.size();

        if (canceled.load(std::memory_order_acquire)) {
            return false;
        }

        // Try to reuse freed slots
        for (size_t i = 0; i < collectionSize; i++) {
            auto& item = subscriptions[i];

            if (const auto pointer = item.lock(); !pointer) {
                // Free slot found
                item = std::move(subscription);
                return true;
            }
        }

        // No free slot found
        subscriptions.push_back(std::move(subscription));
        return true;
    }

    std::vector<std::weak_ptr<Subscription::Impl>> subscriptions{};
    std::atomic_bool canceled{};
    std::mutex mutex{};
};

/// CancellationToken
/// ========================================================================

namespace vanilo::concurrent {

    bool operator==(const CancellationToken& lhs, const CancellationToken& rhs) noexcept
    {
        return lhs._impl == rhs._impl;
    }

    bool operator!=(const CancellationToken& lhs, const CancellationToken& rhs) noexcept
    {
        return !(lhs == rhs);
    }

}

CancellationToken CancellationToken::none()
{
    static CancellationToken token;
    return token;
}

CancellationToken::CancellationToken(): _impl{std::make_shared<Impl>()}
{
}

void CancellationToken::cancel() const
{
    _impl->cancel();
}

bool CancellationToken::isCancellationRequested() const noexcept
{
    return _impl->canceled.load(std::memory_order_acquire);
}

CancellationToken::Subscription CancellationToken::subscribe(std::function<void()> callback) const
{
    Subscription subscription;
    subscription._impl = std::make_shared<Subscription::Impl>(std::move(callback));

    // Directly notify the subscriber that the subscription cancellation has been requested
    if (!_impl->subscribe(subscription._impl)) {
        subscription._impl->notify();
    }

    return subscription;
}

void CancellationToken::throwIfCancellationRequested() const
{
    if (_impl->canceled.load(std::memory_order_acquire)) {
        throw OperationCanceledException();
    }
}

/// Subscription
/// ========================================================================

CancellationToken::Subscription::Subscription() = default;

CancellationToken::Subscription::~Subscription() = default;

void CancellationToken::Subscription::unsubscribe()
{
    if (_impl) {
        _impl->unsubscribe();
        _impl.reset();
    }
}