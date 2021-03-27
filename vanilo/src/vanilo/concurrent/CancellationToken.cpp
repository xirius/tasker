#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/core/Tracer.h>

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
        { // Mutually exclusive with subscribe method
            std::lock_guard<std::mutex> lock{mutex};
            canceled = true;
        }

        // Callbacks have to be called without acquired mutex
        for (auto& subscription : subscriptions) {
            if (auto callback = subscription.lock()) {
                callback->notify();
            }
        }
    }

    /**
     * Registers the callback which is called when the cancellation has been requested.
     * @return False if the cancellation has been requested and the registration cannot be done, true otherwise.
     */
    bool subscribe(std::weak_ptr<CancellationToken::Subscription::Impl> subscription)
    {
        // No additional subscription can be made if the cancellation has been requested
        std::lock_guard<std::mutex> lock{mutex};
        size_t collectionSize = subscriptions.size();

        if (canceled) {
            return false;
        }

        // Try to reuse freed slots
        for (size_t i = index; i < collectionSize; i++) {
            auto& item   = subscriptions[i];
            auto pointer = item.lock();

            if (!pointer) { // Free slot found
                item = std::move(subscription);
                return true;
            }

            index = i;
        }

        // No free slot found
        subscriptions.push_back(std::move(subscription));
        index = 0;
        return true;
    }

    std::vector<std::weak_ptr<CancellationToken::Subscription::Impl>> subscriptions{};
    std::atomic<bool> canceled{};
    std::mutex mutex{};
    size_t index{0};
};

/// CancellationToken
/// ========================================================================

CancellationToken CancellationToken::none()
{
    static CancellationToken token;
    return token;
}

CancellationToken::CancellationToken(): _impl{std::make_shared<Impl>()}
{
}

bool CancellationToken::operator==(const CancellationToken& other) const noexcept
{
    return _impl == other._impl;
}

bool CancellationToken::operator!=(const CancellationToken& other) const noexcept
{
    return _impl != other._impl;
}

void CancellationToken::cancel()
{
    _impl->cancel();
}

bool CancellationToken::isCancellationRequested() const noexcept
{
    return _impl->canceled.load();
}

CancellationToken::Subscription CancellationToken::subscribe(std::function<void()> callback)
{
    Subscription subscription;
    subscription._impl = std::make_shared<CancellationToken::Subscription::Impl>(std::move(callback));

    // Directly notify the subscriber that the subscription cancellation has been requested
    if (!_impl->subscribe(subscription._impl)) {
        subscription._impl->notify();
    }

    return subscription;
}

void CancellationToken::throwIfCancellationRequested()
{
    if (_impl->canceled) {
        throw CanceledException();
    }
}

/// Subscription
/// ========================================================================

CancellationToken::Subscription::Subscription() = default;

CancellationToken::Subscription::~Subscription() = default;

void CancellationToken::Subscription::unsubscribe() const
{
    _impl->unsubscribe();
}
