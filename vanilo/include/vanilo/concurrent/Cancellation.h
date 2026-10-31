#ifndef VANILO_CONCURRENT_CANCELLATION_H
#define VANILO_CONCURRENT_CANCELLATION_H

#include <vanilo/Export.h>

#include <stdexcept>
#include <stop_token>

namespace vanilo::concurrent {

    /**
     * Exception indicating that an operation was canceled.
     *
     * Tasks that never ran because their chain was canceled, or because their executor shut down, deliver it to
     * their onError handlers and their TaskHandle. A task can throw it itself (see throwIfStopRequested) to stop
     * halfway. It is never reported as an unhandled exception.
     */
    class VANILO_EXPORT OperationCanceledException final: public std::runtime_error
    {
      public:
        OperationCanceledException();
    };

    /**
     * Throws an OperationCanceledException if a stop was requested through the token.
     * @param token The token to check.
     */
    VANILO_EXPORT void throwIfStopRequested(const std::stop_token& token);

} // namespace vanilo::concurrent

#endif // VANILO_CONCURRENT_CANCELLATION_H
