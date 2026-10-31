#include <vanilo/tasker/ExecutorCore.h>
#include <vanilo/tasker/Task.h>
#include <vanilo/tasker/UnhandledException.h>

#include <iostream>
#include <mutex>
#include <utility>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

/// Cancellation
/// ================================================================================================

OperationCanceledException::OperationCanceledException(): std::runtime_error{"The operation was canceled."}
{
}

void vanilo::concurrent::throwIfStopRequested(const std::stop_token& token)
{
    if (token.stop_requested()) {
        throw OperationCanceledException{};
    }
}

const std::exception_ptr& detail::canceledException() noexcept
{
    static const std::exception_ptr exception = std::make_exception_ptr(OperationCanceledException{});
    return exception;
}

bool detail::isCancellation(const std::exception_ptr& error) noexcept
{
    if (error == canceledException()) {
        return true;
    }
    if (!error) {
        return false;
    }

    try {
        std::rethrow_exception(error);
    }
    catch (const OperationCanceledException&) {
        return true;
    }
    catch (...) {
        return false;
    }
}

/// Unhandled exceptions
/// ================================================================================================

namespace {
    void writeToStandardError(const std::exception_ptr& exception)
    {
        try {
            std::rethrow_exception(exception);
        }
        catch (const std::exception& ex) {
            std::cerr << "vanilo::tasker: unhandled exception in a task: " << ex.what() << '\n';
        }
        catch (...) {
            std::cerr << "vanilo::tasker: unhandled non-standard exception in a task\n";
        }
    }

    struct UnhandledExceptionState
    {
        std::mutex mutex;
        UnhandledExceptionHandler handler = writeToStandardError;
    };

    // Function-local static, so it is initialised on first use regardless of static initialisation order.
    UnhandledExceptionState& unhandledExceptionState()
    {
        static UnhandledExceptionState state;
        return state;
    }
} // namespace

UnhandledExceptionHandler vanilo::tasker::setUnhandledExceptionHandler(UnhandledExceptionHandler handler)
{
    auto& state = unhandledExceptionState();
    std::lock_guard lock{state.mutex};
    return std::exchange(state.handler, std::move(handler));
}

void detail::reportUnhandledException(const std::exception_ptr& exception) noexcept
{
    if (!exception || isCancellation(exception)) {
        return; // A cancellation is not an error
    }

    UnhandledExceptionHandler handler;
    try {
        auto& state = unhandledExceptionState();
        std::lock_guard lock{state.mutex};
        handler = state.handler;
    }
    catch (...) { // NOSONAR
        return;   // The handler couldn't be copied: nothing to report to
    }

    if (!handler) {
        return;
    }

    try {
        handler(exception);
    }
    catch (...) { // NOSONAR
        // A throwing handler must not take down the executor that reported the exception
    }
}

void detail::runJob(Job& job) noexcept
{
    try {
        job.run();
    }
    catch (...) {
        reportUnhandledException(std::current_exception());
    }
}

/// Posting and chains
/// ================================================================================================

void detail::post(TaskExecutor* executor, Job job)
{
    if (executor) {
        if (const auto& core = ExecutorAccess::core(*executor); core->tryEnqueue(job)) {
            return;
        }
    }
    job.cancel();
}

void detail::ChainParts::checkUsable() const
{
    if (!head) {
        throw std::logic_error{"This task builder was already used: it was submitted or moved from."};
    }
}
