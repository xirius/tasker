#include <vanilo/concurrent/CancellationToken.h>

using namespace vanilo::concurrent;

/// CancellationToken::Impl
/// ========================================================================

struct CancellationToken::Impl
{
    std::atomic<bool> canceled;
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
}

bool CancellationToken::isCanceled() const noexcept
{
    return _impl->canceled.load();
}