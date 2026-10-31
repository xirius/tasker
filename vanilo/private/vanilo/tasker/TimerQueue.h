#ifndef VANILO_TASKER_TIMER_QUEUE_H
#define VANILO_TASKER_TIMER_QUEUE_H

#include <vanilo/Export.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vanilo::tasker::detail {

    using SteadyClock = std::chrono::steady_clock;

    /**
     * Something the timer queue fires when it is due: the start of a delayed chain, or the next run of a
     * periodic one.
     */
    class TimerEntry
    {
      public:
        explicit TimerEntry(const SteadyClock::time_point due) noexcept: _due{due}
        {
        }

        virtual ~TimerEntry() = default;

        TimerEntry(const TimerEntry&) = delete;
        TimerEntry& operator=(const TimerEntry&) = delete;

        [[nodiscard]] SteadyClock::time_point due() const noexcept
        {
            return _due;
        }

        void setDue(const SteadyClock::time_point due) noexcept
        {
            _due = due;
        }

        /**
         * @return The order in which entries with the same due time fire (set by TimerQueue::schedule).
         */
        [[nodiscard]] std::uint64_t sequence() const noexcept
        {
            return _sequence;
        }

        /**
         * @return True if the entry is no longer wanted (its chain was stopped): it is canceled instead of fired.
         */
        [[nodiscard]] virtual bool isCanceled() const noexcept = 0;

        /**
         * The entry is due. Called on the timer thread, outside the queue's lock.
         */
        virtual void fire(std::unique_ptr<TimerEntry> self) noexcept = 0;

        /**
         * The entry will never fire (it was canceled, or the queue shut down).
         */
        virtual void cancel(std::unique_ptr<TimerEntry> self) noexcept = 0;

      private:
        friend class TimerQueue;

        SteadyClock::time_point _due;
        std::uint64_t _sequence{0}; // keeps entries with the same due time in order
    };

    /**
     * The thread that waits for delayed and periodic tasks: a heap of entries ordered by due time. Fired entries
     * only queue tasks on their executors, so a slow task never delays the timer.
     */
    class VANILO_EXPORT TimerQueue final
    {
      public:
        /**
         * The process-wide queue, created on first use and shut down when the program exits.
         */
        static const std::shared_ptr<TimerQueue>& instance();

        TimerQueue();
        ~TimerQueue();

        TimerQueue(const TimerQueue&) = delete;
        TimerQueue& operator=(const TimerQueue&) = delete;

        /**
         * Adds an entry.
         * @return Null if added; the entry back if the queue has shut down.
         */
        std::unique_ptr<TimerEntry> schedule(std::unique_ptr<TimerEntry> entry);

        /**
         * Asks the timer thread to drop the canceled entries (an entry's chain was stopped).
         */
        void purge() noexcept;

        /**
         * Stops the timer thread and cancels the pending entries. Further entries are refused.
         */
        void shutdown() noexcept;

        [[nodiscard]] std::size_t pendingCount() const;

      private:
        void work();

        mutable std::mutex _mutex;
        std::condition_variable _condition;
        std::vector<std::unique_ptr<TimerEntry>> _heap;
        std::uint64_t _sequence{0};
        bool _purgeRequested{false};
        bool _stopped{false};
        std::thread _thread;
    };

} // namespace vanilo::tasker::detail

#endif // VANILO_TASKER_TIMER_QUEUE_H
