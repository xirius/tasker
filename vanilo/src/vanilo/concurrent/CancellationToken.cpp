#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/core/Tracer.h>

#include <map>
#include <mutex>

using namespace vanilo::concurrent;

/// CancellationToken::Impl
/// ========================================================================

struct CancellationToken::Impl
{
    void notify()
    {
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

    void unregister(uintptr_t id)
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

void CancellationToken::cancel() noexcept
{
    _impl->canceled.store(true);
    _impl->notify();
}

bool CancellationToken::isCanceled() const noexcept
{
    return _impl->canceled.load();
}

CancellationToken::RegistrationToken CancellationToken::registerAction(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock{_impl->mutex};
    RegistrationToken token{*this};
    _impl->callbacks.emplace(reinterpret_cast<uintptr_t>(token._impl.get()), std::move(callback));
    return token;
}

/// RegistrationToken::Impl
/// ========================================================================

struct CancellationToken::RegistrationToken::Impl
{
    explicit Impl(CancellationToken& token): object{token._impl}, id{reinterpret_cast<uintptr_t>(this)}
    {
    }

    ~Impl()
    {
        if (auto token = object.lock()) {
            token->unregister(id);
        }
    }

    std::weak_ptr<CancellationToken::Impl> object;
    uintptr_t id;
};

/// RegistrationToken
/// ========================================================================

CancellationToken::RegistrationToken::RegistrationToken(CancellationToken& token): _impl{std::make_unique<RegistrationToken::Impl>(token)}
{
}

CancellationToken::RegistrationToken::~RegistrationToken() = default;
