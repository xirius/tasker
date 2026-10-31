#include <vanilo/tasker/detail/Chain.h>

using namespace vanilo::tasker::detail;

/// ChainControl
/// ================================================================================================

// A stop of these tokens stops the chain. If one is already stopped, its link stops the chain right away.
ChainControl::ChainControl(std::stop_token first, std::stop_token second)
    : _firstLink{std::move(first), StopLink{&_source}}, _secondLink{std::move(second), StopLink{&_source}}
{
}

ChainControl::~ChainControl() = default;

/// Completion
/// ================================================================================================

void Completion::wait() const noexcept
{
    while (_status.load(std::memory_order_acquire) == Pending) {
        _status.wait(Pending, std::memory_order_acquire);
    }
}

bool Completion::waitUntil(const std::chrono::steady_clock::time_point deadline) const
{
    if (isReady()) {
        return true;
    }

    // Timed waits use the condition variable (atomic waits have no timeout). publish() notifies it when it
    // sees a timed waiter; both sides use sequential consistency, so one of them sees the other.
    _timedWaiters.fetch_add(1, std::memory_order_seq_cst);
    bool ready;
    {
        std::unique_lock lock{_mutex};
        ready = _condition.wait_until(lock, deadline, [this] { return _status.load(std::memory_order_seq_cst) != Pending; });
    }
    _timedWaiters.fetch_sub(1, std::memory_order_relaxed);
    return ready;
}

void Completion::releaseHandle(const bool observed) noexcept
{
    const auto previous = _flags.fetch_or(HandleReleased | (observed ? Observed : 0), std::memory_order_acq_rel);

    if ((previous & ErrorUnhandled) && !observed) {
        reportUnhandledException(_error); // nobody received the error, and now nobody can
    }
}

void Completion::publishValue() noexcept
{
    publish();
}

void Completion::publishError(const std::exception_ptr& error, const bool reportIfUnobserved) noexcept
{
    _error = error;
    publish();

    if (reportIfUnobserved) {
        const auto previous = _flags.fetch_or(ErrorUnhandled, std::memory_order_acq_rel);
        if ((previous & HandleReleased) && !(previous & Observed)) {
            reportUnhandledException(_error); // the handle is already gone
        }
    }
}

void Completion::publish() noexcept
{
    _status.store(Done, std::memory_order_seq_cst);
    _status.notify_all();

    if (_timedWaiters.load(std::memory_order_seq_cst) > 0) {
        const std::lock_guard lock{_mutex};
        _condition.notify_all();
    }
}
