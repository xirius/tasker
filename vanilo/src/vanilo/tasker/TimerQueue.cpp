#include <vanilo/tasker/TaskBuilder.h>
#include <vanilo/tasker/TimerQueue.h>

#include <algorithm>
#include <atomic>

using namespace vanilo::tasker;
using namespace vanilo::tasker::detail;

namespace {

    // A min-heap on (due, sequence): std:: heap functions keep the "largest" element at the front.
    bool laterThan(const std::unique_ptr<TimerEntry>& left, const std::unique_ptr<TimerEntry>& right) noexcept;

    /**
     * Wakes the timer queue when a chain waiting in it is stopped, so it drops the entry at once.
     */
    struct WakeTimerQueue
    {
        std::weak_ptr<TimerQueue> queue;

        void operator()() const noexcept
        {
            if (const auto timerQueue = queue.lock()) {
                timerQueue->purge();
            }
        }
    };

    /// Delayed chains
    /// ============================================================================================

    /**
     * The start of a delayed chain.
     */
    class DelayedStart final: public TimerEntry
    {
      public:
        DelayedStart(
            const SteadyClock::time_point due,
            std::unique_ptr<Node> head,
            std::shared_ptr<ChainControl> chain,
            std::weak_ptr<TimerQueue> queue)
            : TimerEntry{due}, _head{std::move(head)}, _chain{std::move(chain)}, _wake{_chain->token(), WakeTimerQueue{std::move(queue)}}
        {
        }

        [[nodiscard]] bool isCanceled() const noexcept override
        {
            return _chain->stopRequested();
        }

        void fire(std::unique_ptr<TimerEntry> self) noexcept override
        {
            auto head = std::move(_head);
            auto chain = std::move(_chain);
            self.reset(); // unregisters the wake-up before the chain goes on

            Node* first = head.get();
            first->start(std::move(head), std::move(chain));
        }

        void cancel(std::unique_ptr<TimerEntry> self) noexcept override
        {
            auto head = std::move(_head);
            auto chain = std::move(_chain);
            self.reset();

            Node::fail(std::move(head), 0, std::move(chain), canceledException(), nullptr, false);
        }

      private:
        std::unique_ptr<Node> _head;
        std::shared_ptr<ChainControl> _chain;
        std::stop_callback<WakeTimerQueue> _wake;
    };

    /// Periodic chains
    /// ============================================================================================

    class SeriesEntry;

    /**
     * The state of a periodic chain, shared by its handle, its timer entry and its runs. Each run is a copy of
     * the chain whose control is this state: complete() and fail() hear about each run. The handle's outcome is
     * published when the series ends.
     */
    class SeriesState final: public ChainState<void>, public std::enable_shared_from_this<SeriesState>
    {
      public:
        SeriesState(ChainParts parts, const std::shared_ptr<TimerQueue>& queue)
            : ChainState<void>{std::move(parts.token)}, _template{std::move(parts.head)}, _period{parts.period}, _mode{parts.mode},
              _queue{queue}, _wake{token(), WakeTimerQueue{queue}}
        {
        }

        void complete(void* /*value*/) noexcept override
        {
            runFinished();
        }

        void fail(const std::exception_ptr& error, const bool handled) noexcept override
        {
            if (!handled) {
                reportUnhandledException(error); // ignores cancellations
            }
            runFinished();
        }

        /**
         * The series' timer entry is due: start a run, and schedule the next one.
         */
        void tick(std::unique_ptr<TimerEntry> entry) noexcept;

        /**
         * The series is over (canceled, or its executor or the timer queue shut down).
         */
        void end() noexcept
        {
            if (_ended.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            std::unique_ptr<Node> chainTemplate;
            std::unique_ptr<TimerEntry> parked;
            {
                std::lock_guard lock{_mutex};
                chainTemplate = std::move(_template);
                parked = std::move(_parked);
            }

            publishError(canceledException(), false);
        }

      private:
        void runFinished() noexcept;
        void scheduleNext(std::unique_ptr<TimerEntry> entry, SteadyClock::time_point due) noexcept;

        std::mutex _mutex;
        std::unique_ptr<Node> _template;     // what each run copies; released when the series ends
        std::unique_ptr<TimerEntry> _parked; // fixed delay: the entry, while a run is in progress
        std::chrono::steady_clock::duration _period;
        PeriodicMode _mode;
        std::weak_ptr<TimerQueue> _queue;
        std::atomic<bool> _ended{false};
        std::stop_callback<WakeTimerQueue> _wake; // last: it may run as soon as it is constructed
    };

    /**
     * The timer entry of a series.
     */
    class SeriesEntry final: public TimerEntry
    {
      public:
        SeriesEntry(const SteadyClock::time_point due, std::shared_ptr<SeriesState> series) noexcept
            : TimerEntry{due}, _series{std::move(series)}
        {
        }

        [[nodiscard]] bool isCanceled() const noexcept override
        {
            return _series->stopRequested();
        }

        void fire(std::unique_ptr<TimerEntry> self) noexcept override
        {
            const auto series = _series; // self may be destroyed by tick()
            series->tick(std::move(self));
        }

        void cancel(std::unique_ptr<TimerEntry> self) noexcept override
        {
            const auto series = std::move(_series);
            self.reset();
            series->end();
        }

      private:
        std::shared_ptr<SeriesState> _series;
    };

    void SeriesState::tick(std::unique_ptr<TimerEntry> entry) noexcept
    {
        if (stopRequested()) {
            entry.reset();
            end();
            return;
        }

        const auto due = entry->due();
        std::unique_ptr<Node> run;
        try {
            std::lock_guard lock{_mutex};
            if (_template) {
                run = _template->cloneChain();
            }
        }
        catch (...) {
            reportUnhandledException(std::current_exception()); // this run is skipped
        }

        if (_mode == PeriodicMode::FixedDelay) {
            if (!run) {
                scheduleNext(std::move(entry), SteadyClock::now() + _period);
                return;
            }

            // Parked before the run starts: the run may finish (and look for it) at once.
            {
                std::lock_guard lock{_mutex};
                _parked = std::move(entry);
            }

            if (!Node::tryStart(std::move(run), shared_from_this())) {
                end(); // the executor is gone
            }
            return;
        }

        if (run && !Node::tryStart(std::move(run), shared_from_this())) {
            entry.reset();
            end(); // the executor is gone
            return;
        }

        // Fixed rate, drift-free: the next run is a whole number of periods after this one, skipping the
        // periods that were missed.
        const auto now = SteadyClock::now();
        auto next = due + _period;
        if (next <= now) {
            next += _period * ((now - next) / _period + 1);
        }
        scheduleNext(std::move(entry), next);
    }

    void SeriesState::runFinished() noexcept
    {
        if (_mode != PeriodicMode::FixedDelay) {
            return;
        }

        std::unique_ptr<TimerEntry> entry;
        {
            std::lock_guard lock{_mutex};
            entry = std::move(_parked);
        }

        if (!entry) {
            return; // the series has ended
        }

        if (stopRequested()) {
            entry.reset();
            end();
            return;
        }

        scheduleNext(std::move(entry), SteadyClock::now() + _period);
    }

    void SeriesState::scheduleNext(std::unique_ptr<TimerEntry> entry, const SteadyClock::time_point due) noexcept
    {
        entry->setDue(due);

        if (const auto queue = _queue.lock()) {
            try {
                entry = queue->schedule(std::move(entry));
            }
            catch (...) {
                reportUnhandledException(std::current_exception());
            }
        }

        if (entry) { // refused: the timer queue has shut down
            entry.reset();
            end();
        }
    }

    bool laterThan(const std::unique_ptr<TimerEntry>& left, const std::unique_ptr<TimerEntry>& right) noexcept
    {
        if (left->due() != right->due()) {
            return left->due() > right->due();
        }
        return left->sequence() > right->sequence();
    }

} // namespace

/// TimerQueue
/// ================================================================================================

const std::shared_ptr<TimerQueue>& TimerQueue::instance()
{
    // Created on first use, so its thread doesn't start when the library is loaded. It is shut down when the
    // program exits but never destroyed, so code running during static destruction can still use it: it then
    // refuses new entries, which cancels their chains.
    static const auto* const queue = new std::shared_ptr<TimerQueue>(std::make_shared<TimerQueue>());

    static const struct Shutdown
    {
        ~Shutdown()
        {
            (*queue)->shutdown();
        }
    } shutdown;

    return *queue;
}

TimerQueue::TimerQueue()
{
    _thread = std::thread{[this] { work(); }};
}

TimerQueue::~TimerQueue()
{
    shutdown();
}

std::unique_ptr<TimerEntry> TimerQueue::schedule(std::unique_ptr<TimerEntry> entry)
{
    bool wake;
    {
        std::lock_guard lock{_mutex};

        if (_stopped) {
            return entry;
        }

        entry->_sequence = _sequence++;
        // The timer thread only needs waking up if the new entry is due before the one it waits for.
        wake = _heap.empty() || entry->due() < _heap.front()->due();

        if (entry->isCanceled()) {
            _purgeRequested = true;
            wake = true;
        }

        _heap.push_back(std::move(entry));
        std::push_heap(_heap.begin(), _heap.end(), laterThan);
    }

    if (wake) {
        _condition.notify_one();
    }
    return nullptr;
}

void TimerQueue::purge() noexcept
{
    {
        std::lock_guard lock{_mutex};
        _purgeRequested = true;
    }
    _condition.notify_one();
}

void TimerQueue::shutdown() noexcept
{
    {
        std::lock_guard lock{_mutex};
        if (_stopped) {
            return;
        }
        _stopped = true;
    }
    _condition.notify_one();

    if (_thread.joinable()) {
        if (_thread.get_id() == std::this_thread::get_id()) {
            _thread.detach();
        }
        else {
            _thread.join();
        }
    }

    // The pending entries will never fire. They are canceled outside the lock: their chains may schedule more.
    std::vector<std::unique_ptr<TimerEntry>> pending;
    {
        std::lock_guard lock{_mutex};
        pending.swap(_heap);
    }

    for (auto& entry : pending) {
        TimerEntry* raw = entry.get();
        raw->cancel(std::move(entry));
    }
}

std::size_t TimerQueue::pendingCount() const
{
    std::lock_guard lock{_mutex};
    return _heap.size();
}

void TimerQueue::work()
{
    std::vector<std::unique_ptr<TimerEntry>> ready;
    std::unique_lock lock{_mutex};

    while (!_stopped) {
        if (_purgeRequested) {
            _purgeRequested = false;

            const auto kept = std::partition(_heap.begin(), _heap.end(), [](const auto& entry) { return !entry->isCanceled(); });
            std::move(kept, _heap.end(), std::back_inserter(ready));
            _heap.erase(kept, _heap.end());
            std::make_heap(_heap.begin(), _heap.end(), laterThan);
        }
        else if (_heap.empty()) {
            _condition.wait(lock);
            continue;
        }
        else if (const auto due = _heap.front()->due(); due > SteadyClock::now()) {
            _condition.wait_until(lock, due);
            continue;
        }
        else {
            const auto now = SteadyClock::now();
            while (!_heap.empty() && _heap.front()->due() <= now) {
                std::pop_heap(_heap.begin(), _heap.end(), laterThan);
                ready.push_back(std::move(_heap.back()));
                _heap.pop_back();
            }
        }

        // Fired and canceled outside the lock: entries queue tasks and may schedule themselves again.
        lock.unlock();
        for (auto& entry : ready) {
            TimerEntry* raw = entry.get();
            if (raw->isCanceled()) {
                raw->cancel(std::move(entry));
            }
            else {
                raw->fire(std::move(entry));
            }
        }
        ready.clear();
        lock.lock();
    }
}

/// Starting chains
/// ================================================================================================

void detail::startChain(ChainParts parts, std::shared_ptr<ChainControl> control)
{
    if (parts.delay <= SteadyClock::duration::zero()) {
        Node* head = parts.head.get();
        head->start(std::move(parts.head), std::move(control));
        return;
    }

    const auto& queue = TimerQueue::instance();
    auto entry = std::make_unique<DelayedStart>(SteadyClock::now() + parts.delay, std::move(parts.head), std::move(control), queue);

    if (auto refused = queue->schedule(std::move(entry))) {
        TimerEntry* raw = refused.get();
        raw->cancel(std::move(refused)); // the program is exiting
    }
}

std::shared_ptr<ChainState<void>> detail::startSeries(ChainParts parts)
{
    const auto& queue = TimerQueue::instance();
    const auto due = SteadyClock::now() + std::max(parts.delay, SteadyClock::duration::zero());
    auto series = std::make_shared<SeriesState>(std::move(parts), queue);

    if (auto refused = queue->schedule(std::make_unique<SeriesEntry>(due, series))) {
        refused.reset();
        series->end(); // the program is exiting
    }
    return series;
}
