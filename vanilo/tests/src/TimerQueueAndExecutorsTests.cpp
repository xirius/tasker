#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>
#include <vanilo/tasker/TimerQueue.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

using namespace vanilo::tasker;
using namespace vanilo::tasker::detail;
using namespace std::chrono_literals;
using vanilo::tests::waitFor;

namespace {
    /**
     * A timer entry calling back when it fires or is canceled; a periodic one schedules itself again.
     */
    class TestEntry final: public TimerEntry
    {
      public:
        TestEntry(
            const SteadyClock::time_point due,
            std::function<void()> onFire,
            std::function<void()> onCancel = [] {},
            std::optional<SteadyClock::duration> period = {},
            TimerQueue* queue = nullptr)
            : TimerEntry{due}, _onFire{std::move(onFire)}, _onCancel{std::move(onCancel)}, _period{period}, _queue{queue}
        {
        }

        void setCanceled() noexcept
        {
            _canceled = true;
        }

        [[nodiscard]] bool isCanceled() const noexcept override
        {
            return _canceled;
        }

        void fire(std::unique_ptr<TimerEntry> self) noexcept override
        {
            _onFire();
            if (_period && _queue) {
                self->setDue(due() + *_period);
                _queue->schedule(std::move(self));
            }
        }

        void cancel(std::unique_ptr<TimerEntry> /*self*/) noexcept override
        {
            _onCancel();
        }

      private:
        std::function<void()> _onFire;
        std::function<void()> _onCancel;
        std::optional<SteadyClock::duration> _period;
        TimerQueue* _queue;
        std::atomic<bool> _canceled{false};
    };

    struct CounterTask final: Task
    {
        explicit CounterTask(std::atomic_int& counter): counter(counter)
        {
        }

        void cancel() noexcept override
        {
            // Ignore
        }

        void run() override
        {
            ++counter;
        }

        std::atomic_int& counter;
    };
} // namespace

TEST_CASE("The timer queue fires by earliest due time, then in scheduling order", "[timer]")
{
    TimerQueue queue;
    std::vector<int> order;
    std::mutex mutex;

    const auto now = SteadyClock::now();
    auto record = [&order, &mutex](const int tag) {
        return [&order, &mutex, tag] {
            std::lock_guard lock{mutex};
            order.push_back(tag);
        };
    };

    // The second and third are due at the same time: they fire in the order they were scheduled.
    queue.schedule(std::make_unique<TestEntry>(now + 60ms, record(1)));
    queue.schedule(std::make_unique<TestEntry>(now + 40ms, record(2)));
    queue.schedule(std::make_unique<TestEntry>(now + 40ms, record(3)));

    REQUIRE(waitFor([&] {
        std::lock_guard lock{mutex};
        return order.size() == 3;
    }));
    REQUIRE(order == std::vector<int>{2, 3, 1});
}

TEST_CASE("The timer queue fires an entry that is due at once", "[timer]")
{
    TimerQueue queue;
    std::atomic_int fired{0};

    queue.schedule(std::make_unique<TestEntry>(SteadyClock::now(), [&fired] { ++fired; }));

    REQUIRE(waitFor([&fired] { return fired == 1; }));
}

TEST_CASE("A timer entry can schedule itself again", "[timer][periodic]")
{
    TimerQueue queue;
    std::atomic_int fired{0};

    queue.schedule(std::make_unique<TestEntry>(SteadyClock::now() + 20ms, [&fired] { ++fired; }, [] {}, 40ms, &queue));

    REQUIRE(waitFor([&fired] { return fired >= 3; }));
}

TEST_CASE("A canceled timer entry is canceled, not fired", "[timer][cancel]")
{
    TimerQueue queue;
    std::atomic_int fired{0};
    std::atomic_int canceled{0};

    SECTION("canceled before it is scheduled")
    {
        auto entry = std::make_unique<TestEntry>(SteadyClock::now() + 1h, [&fired] { ++fired; }, [&canceled] { ++canceled; });
        entry->setCanceled();
        queue.schedule(std::move(entry)); // a canceled entry asks for a purge: it doesn't wait for its due time
    }

    SECTION("canceled while waiting, then purged")
    {
        auto entry = std::make_unique<TestEntry>(SteadyClock::now() + 1h, [&fired] { ++fired; }, [&canceled] { ++canceled; });
        auto* raw = entry.get();
        queue.schedule(std::move(entry));
        raw->setCanceled();
        queue.purge();
    }

    SECTION("canceled when it is due")
    {
        auto entry = std::make_unique<TestEntry>(SteadyClock::now() + 20ms, [&fired] { ++fired; }, [&canceled] { ++canceled; });
        entry->setCanceled();
        queue.schedule(std::move(entry));
    }

    REQUIRE(waitFor([&canceled] { return canceled == 1; }));
    REQUIRE(fired == 0);
    REQUIRE(queue.pendingCount() == 0);
}

TEST_CASE("ManualExecutor runs tasks and respects maxCount", "[executor][manual]")
{
    const auto executor = ManualExecutor::create();
    std::atomic_int counter{0};

    for (int i = 0; i < 5; ++i) {
        executor->submit(std::make_unique<CounterTask>(counter));
    }

    // Run only 3 tasks
    auto remaining = executor->runPending(3);
    REQUIRE(counter.load() == 3);
    REQUIRE(remaining == 2);

    // Run the rest
    remaining = executor->runPending(10);
    REQUIRE(counter.load() == 5);
    REQUIRE(remaining == 0);
}

TEST_CASE("ManualExecutor::runUntil() stops on a stop request", "[executor][manual][token]")
{
    const auto executor = ManualExecutor::create();
    std::stop_source stopSource;
    std::atomic_int counter{0};

    for (int i = 0; i < 3; ++i) {
        executor->submit(std::make_unique<CounterTask>(counter));
    }

    std::thread stopper([&stopSource] {
        std::this_thread::sleep_for(30ms);
        stopSource.request_stop();
    });

    const auto remaining = executor->runUntil(stopSource.get_token());
    stopper.join();

    // The tasks queued before the stop request ran; nothing is left
    REQUIRE(counter == 3);
    REQUIRE(remaining == 0);
}

TEST_CASE("ThreadPoolExecutor runs tasks and can resize", "[executor][pool]")
{
    const auto threadPool = ThreadPoolExecutor::create(2);
    std::atomic_int counter{0};

    for (int i = 0; i < 10; ++i) {
        threadPool->submit(std::make_unique<CounterTask>(counter));
    }

    REQUIRE(waitFor([&counter] { return counter == 10; }));

    // Resize up and then down, ensure the futures resolve
    auto resizeFutureIncrease = threadPool->resize(4);
    REQUIRE_NOTHROW(resizeFutureIncrease.get());
    REQUIRE(threadPool->threadCount() == 4);

    auto resizeFutureDecrease = threadPool->resize(1);
    REQUIRE_NOTHROW(resizeFutureDecrease.get());
    REQUIRE(threadPool->threadCount() == 1);

    for (int i = 0; i < 5; ++i) {
        threadPool->submit(std::make_unique<CounterTask>(counter));
    }

    REQUIRE(waitFor([&counter] { return counter == 15; }));
}
