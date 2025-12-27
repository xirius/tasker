#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/core/Tracer.h>

#include <algorithm>
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

    void unsubscribe() noexcept
    {
        std::lock_guard lock{mutex};
        _callback = nullptr;
    }

    void notify() noexcept
    {
        std::function<void()> callback;
        {
            std::lock_guard lock{mutex};
            callback = std::move(_callback); // move under lock => no data race and future calls are no-op
        }

        if (!callback) {
            return;
        }

        try {
            callback();
        }
        catch (const std::exception& ex) {
            TRACE("An unhandled exception occurred during cancellation notification. Message: %s", ex.what());
        }
        catch (...) {
            TRACE("An unhandled exception occurred during cancellation notification!");
        }
    }

    std::function<void()> _callback;
    std::mutex mutex{};
};

/// Subscription
/// ========================================================================

CancellationToken::Subscription::Subscription(std::shared_ptr<Impl> impl) noexcept: _impl{std::move(impl)}
{
}

CancellationToken::Subscription::~Subscription()
{
    unsubscribe();
}

void CancellationToken::Subscription::unsubscribe() noexcept
{
    if (_impl) {
        _impl->unsubscribe();
        _impl.reset();
    }
}

/// CancellationToken::Impl
/// ========================================================================

struct CancellationToken::Impl
{
    void cancel()
    {
        // One-shot cancellation
        if (canceled.exchange(true, std::memory_order_release)) {
            return; // already canceled
        }

        // Snapshot under lock to avoid vector races
        std::vector<std::shared_ptr<Subscription::Impl>> snapshot;
        {
            std::lock_guard lock{mutex};
            snapshot.reserve(subscriptions.size());

            for (auto& subscription : subscriptions) {
                if (auto callback = subscription.lock()) {
                    snapshot.push_back(callback);
                }
            }
        }

        // Notify without holding the subscriptions mutex
        for (const auto& subscription : snapshot) {
            subscription->notify();
        }
    }

    [[nodiscard]] bool isCanceled() const noexcept
    {
        return canceled.load(std::memory_order_acquire);
    }

    /**
     * Registers the callback which is called when the cancellation has been requested.
     * @return False if the cancellation has been requested and the registration cannot be done, true otherwise.
     */
    bool addSubscription(std::weak_ptr<Subscription::Impl> subscription)
    {
        // No additional subscription can be made if the cancellation has been requested
        std::lock_guard lock{mutex};

        if (canceled.load(std::memory_order_acquire)) {
            return false;
        }

        // 1) Try the free-list first (validate entries)
        while (!freeSlots.empty()) {
            const size_t idx = freeSlots.back();
            freeSlots.pop_back();

            if (idx < subscriptions.size()) {
                if (auto& slot = subscriptions[idx]; slot.expired()) {
                    slot = std::move(subscription);
                    const size_t size = subscriptions.size();
                    probeIndex = size ? (idx + 1) % size : 0;
                    return true;
                }
            }
            // stale entry -> skip
        }

        const size_t subscriptionsSize = subscriptions.size();

        // 2) Bounded scan around probeIndex; collect a few extra free slots on the way
        const size_t maxScan = subscriptionsSize > 0 ? std::min<size_t>(subscriptionsSize, kMaxScan) : 0;
        auto chosen = static_cast<size_t>(-1);

        for (size_t scanned = 0; scanned < maxScan; ++scanned) {
            size_t slotIndex = probeIndex + scanned;
            if (slotIndex >= subscriptionsSize) {
                slotIndex -= subscriptionsSize; // wrap
            }

            if (auto& slot = subscriptions[slotIndex]; slot.expired()) {
                if (chosen == static_cast<size_t>(-1)) {
                    chosen = slotIndex; // use this one for the current insertion
                }
                else if (freeSlots.size() < kMaxFree) {
                    freeSlots.push_back(slotIndex); // remember for later
                }
            }
        }

        if (chosen != static_cast<size_t>(-1)) {
            subscriptions[chosen] = std::move(subscription);
            probeIndex = subscriptionsSize ? (chosen + 1) % subscriptionsSize : 0;
            return true;
        }

        // 3) Full scan (once in a while) and collect some free slots
        for (size_t i = 0; i < subscriptionsSize; ++i) {
            if (auto& slot = subscriptions[i]; slot.expired()) {
                if (chosen == static_cast<size_t>(-1)) {
                    chosen = i;
                }
                else if (freeSlots.size() < kMaxFree) {
                    freeSlots.push_back(i);
                }
            }
        }

        if (chosen != static_cast<size_t>(-1)) {
            subscriptions[chosen] = std::move(subscription);
            probeIndex = subscriptionsSize ? (chosen + 1) % subscriptionsSize : 0;
            return true;
        }

        // 4) No free slot found -> append
        subscriptions.push_back(std::move(subscription));
        probeIndex = 0;
        return true;
    }

    std::vector<std::weak_ptr<Subscription::Impl>> subscriptions{};
    std::atomic_bool canceled{};
    std::mutex mutex{};
    size_t probeIndex{0};
    std::vector<size_t> freeSlots{};

    static constexpr size_t kMaxScan = 16;
    static constexpr size_t kMaxFree = 16;
};

/// CancellationToken
/// ========================================================================

namespace vanilo::concurrent {

    bool operator==(const CancellationToken& a, const CancellationToken& b) noexcept
    {
        return a._impl == b._impl;
    }

    bool operator!=(const CancellationToken& a, const CancellationToken& b) noexcept
    {
        return !(a == b);
    }

}

CancellationToken::CancellationToken(std::shared_ptr<Impl> impl) noexcept: _impl(std::move(impl))
{
}

CancellationToken CancellationToken::none()
{
    return CancellationToken{};
}

bool CancellationToken::isCancellationRequested() const noexcept
{
    // none() token => never canceled
    return _impl ? _impl->isCanceled() : false;
}

CancellationToken::Subscription CancellationToken::subscribe(std::function<void()> callback) const
{
    // none() token => do nothing and return an empty subscription
    if (!_impl) {
        return Subscription{};
    }

    auto node = std::make_shared<Subscription::Impl>(std::move(callback));

    // Register first, then if already canceled, notify immediately.
    // This ensures consistent behavior even under races.
    if (!_impl->addSubscription(node)) {
        node->notify();
        return Subscription{std::move(node)};
    }

    return Subscription{std::move(node)};
}

void CancellationToken::throwIfCancellationRequested() const
{
    if (isCancellationRequested()) {
        throw OperationCanceledException();
    }
}

/// CancellationTokenSource
/// ========================================================================

CancellationTokenSource::CancellationTokenSource(): _impl{std::make_shared<CancellationToken::Impl>()}
{
}

void CancellationTokenSource::cancel() const noexcept
{
    _impl->cancel();
}

bool CancellationTokenSource::isCancellationRequested() const noexcept
{
    return _impl->isCanceled();
}

CancellationToken CancellationTokenSource::token() const noexcept
{
    return CancellationToken{_impl};
}
