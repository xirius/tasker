#ifndef VANILO_TESTS_TEST_UTILITIES_H
#define VANILO_TESTS_TEST_UTILITIES_H

#include <vanilo/tasker/Tasker.h>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace vanilo::tests {

    using namespace std::chrono_literals;

    /**
     * Polls the predicate until it is true or the timeout expires.
     * @return True if the predicate became true.
     */
    template <typename Predicate>
    bool waitFor(Predicate predicate, const std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    /**
     * Runs the function on a detached thread and reports whether it finished in time, so that a hang
     * fails the test instead of blocking the whole test run.
     */
    template <typename Function>
    bool runsWithin(Function function, const std::chrono::seconds timeout)
    {
        auto finished = std::make_shared<std::promise<void>>();
        auto future = finished->get_future();
        std::thread{[finished, function]() mutable {
            function();
            finished->set_value();
        }}.detach();
        return future.wait_for(timeout) == std::future_status::ready;
    }

    /**
     * Records the exceptions passed to the unhandled-exception handler while it is alive, then restores
     * the previous handler.
     */
    class UnhandledExceptions
    {
      public:
        UnhandledExceptions()
            : _previous{tasker::setUnhandledExceptionHandler([this](const std::exception_ptr& exception) {
                  std::string message = "<unknown>";
                  try {
                      std::rethrow_exception(exception);
                  }
                  catch (const std::exception& ex) {
                      message = ex.what();
                  }
                  catch (...) { // NOSONAR
                      // Keep "<unknown>"
                  }
                  std::lock_guard lock{_mutex};
                  _messages.push_back(std::move(message));
              })}
        {
        }

        ~UnhandledExceptions()
        {
            tasker::setUnhandledExceptionHandler(std::move(_previous));
        }

        UnhandledExceptions(const UnhandledExceptions&) = delete;
        UnhandledExceptions& operator=(const UnhandledExceptions&) = delete;

        [[nodiscard]] std::vector<std::string> messages() const
        {
            std::lock_guard lock{_mutex};
            return _messages;
        }

      private:
        mutable std::mutex _mutex;
        std::vector<std::string> _messages;
        tasker::UnhandledExceptionHandler _previous;
    };

    /**
     * Captures what is written to std::cerr while it is alive.
     */
    class StandardErrorCapture
    {
      public:
        StandardErrorCapture(): _previous{std::cerr.rdbuf(_buffer.rdbuf())}
        {
        }

        ~StandardErrorCapture()
        {
            std::cerr.rdbuf(_previous);
        }

        StandardErrorCapture(const StandardErrorCapture&) = delete;
        StandardErrorCapture& operator=(const StandardErrorCapture&) = delete;

        [[nodiscard]] std::string text() const
        {
            return _buffer.str();
        }

      private:
        std::ostringstream _buffer;
        std::streambuf* _previous;
    };

    /**
     * A plain Task that records whether it was canceled and how often it ran.
     */
    class RecordingTask final: public tasker::Task
    {
      public:
        RecordingTask(std::atomic<int>& runs, std::atomic<bool>& canceled): _runs{runs}, _canceled{canceled}
        {
        }

        void cancel() noexcept override
        {
            _canceled = true;
        }

        void run() override
        {
            ++_runs;
        }

      private:
        std::atomic<int>& _runs;
        std::atomic<bool>& _canceled;
    };

} // namespace vanilo::tests

#endif // VANILO_TESTS_TEST_UTILITIES_H
