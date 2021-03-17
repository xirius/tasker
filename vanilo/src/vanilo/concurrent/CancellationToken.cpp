#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/core/Tracer.h>

#include <map>
#include <mutex>

using namespace vanilo::concurrent;

/// CancellationToken::Impl
/// ========================================================================

struct CancellationToken::Impl
{
    void cancel()
    {
        canceled.store(true);

        std::lock_guard<std::mutex> lock{mutex};
        for (auto& pair : callbacks) {
            try {
                pair.second();
            }
            catch (const std::exception& ex) {
                TRACE("An unhandled exception occurred during cancellation notification. Message: %s", ex.what());
            }
            catch (...) {
                TRACE("An unhandled exception occurred during cancellation notification!");
            }
        }
    }

    template <typename Callback>
    void subscribe(uintptr_t id, Callback&& callback)
    {
        std::lock_guard<std::mutex> lock{mutex};
        callbacks.emplace(id, std::forward<Callback>(callback));
    }

    void unsubscribe(uintptr_t id)
    {
        std::lock_guard<std::mutex> lock{mutex};
        callbacks.erase(id);
    }

    std::atomic<bool> canceled{};
    std::map<uintptr_t, std::function<void()>> callbacks{};
    std::mutex mutex{};
};

/// CancellationToken
/// ========================================================================

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

bool CancellationToken::isCanceled() const noexcept
{
    return _impl->canceled.load();
}

CancellationToken::Subscription CancellationToken::subscribe(std::function<void()> callback)
{
    Subscription token{*this};
    _impl->subscribe(reinterpret_cast<uintptr_t>(token._impl.get()), std::move(callback));
    return token;
}

/// Subscription::Impl
/// ========================================================================

struct CancellationToken::Subscription::Impl
{
    explicit Impl(CancellationToken& token): object{token._impl}, id{reinterpret_cast<uintptr_t>(this)}
    {
    }

    ~Impl()
    {
        unsubscribe();
    }

    void unsubscribe() const
    {
        if (auto token = object.lock()) {
            token->unsubscribe(id);
        }
    }

    std::weak_ptr<CancellationToken::Impl> object;
    uintptr_t id;
};

/// Subscription
/// ========================================================================

CancellationToken::Subscription::Subscription(CancellationToken& token): _impl{std::make_unique<Subscription::Impl>(token)}
{
}

CancellationToken::Subscription::~Subscription() = default;

void CancellationToken::Subscription::unsubscribe() const
{
    _impl->unsubscribe();
}
