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
        canceled.store(true);

        for (auto& subscription : subscriptions) {
            if (auto object = subscription.lock()) {
                object->notify();
            }
        }
    }

    void subscribe(std::weak_ptr<CancellationToken::Subscription::Impl> subscription)
    {
        std::lock_guard<std::mutex> lock{mutex};

        // No additional subscription can be made if the cancellation has been requested
        if (!canceled) {
            subscriptions.push_back(std::move(subscription));
        }
    }

    std::vector<std::weak_ptr<CancellationToken::Subscription::Impl>> subscriptions{};
    std::atomic<bool> canceled{};
    std::mutex mutex{};
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
    Subscription token;
    token._impl = std::make_shared<CancellationToken::Subscription::Impl>(std::move(callback));
    _impl->subscribe(token._impl);
    return token;
}

/// Subscription
/// ========================================================================

CancellationToken::Subscription::Subscription() = default;

CancellationToken::Subscription::~Subscription() = default;

void CancellationToken::Subscription::unsubscribe() const
{
    _impl->unsubscribe();
}
