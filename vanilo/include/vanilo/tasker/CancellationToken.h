#pragma once

#include <atomic>
#include <memory>

namespace tasker {

class CancellationToken
{
  public:
    CancellationToken();

    bool operator==(const CancellationToken& other) const noexcept;
    bool operator!=(const CancellationToken& other) const noexcept;

    void cancel() noexcept;
    bool isCanceled() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};

/// CancellationToken::Impl
/// ========================================================================

struct CancellationToken::Impl
{
    std::atomic<bool> canceled;
};

/// CancellationToken
/// ========================================================================

inline CancellationToken::CancellationToken(): _impl{std::make_shared<Impl>()}
{
}

inline bool CancellationToken::operator==(const CancellationToken& other) const noexcept
{
    return _impl == other._impl;
}

inline bool CancellationToken::operator!=(const CancellationToken& other) const noexcept
{
    return _impl != other._impl;
}

inline void CancellationToken::cancel() noexcept
{
    _impl->canceled.store(true);
}

inline bool CancellationToken::isCanceled() const noexcept
{
    return _impl->canceled.load();
}

} // namespace tasker
