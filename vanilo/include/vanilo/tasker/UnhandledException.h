#ifndef VANILO_TASKER_UNHANDLED_EXCEPTION_H
#define VANILO_TASKER_UNHANDLED_EXCEPTION_H

#include <vanilo/Export.h>

#include <exception>
#include <functional>

namespace vanilo::tasker {

    /**
     * Receives an exception that escaped a task and that nobody handled: thrown by a posted function, by a task
     * whose error no onError handler received while nobody looked at its TaskHandle, by a run of a periodic task
     * without an onError handler for it, or by a handler itself. Cancellations (OperationCanceledException) are
     * never reported. It may be called concurrently from any executor thread.
     */
    using UnhandledExceptionHandler = std::function<void(const std::exception_ptr&)>;

    /**
     * Sets the process-wide handler for unhandled task exceptions. The default handler writes the exception's
     * message to stderr; an empty handler ignores unhandled exceptions.
     * @param handler The new handler.
     * @return The previous handler.
     */
    VANILO_EXPORT UnhandledExceptionHandler setUnhandledExceptionHandler(UnhandledExceptionHandler handler);

} // namespace vanilo::tasker

#endif // VANILO_TASKER_UNHANDLED_EXCEPTION_H
