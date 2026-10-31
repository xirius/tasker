#include <vanilo/concurrent/ConcurrentQueue.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace vanilo::concurrent;
using namespace std::chrono_literals;

TEST_CASE("ConcurrentQueue basic operations", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<int> queue;

    SECTION("Initial state")
    {
        REQUIRE(queue.empty());
        REQUIRE(queue.size() == 0);
        REQUIRE_FALSE(queue.isClosed());
    }

    SECTION("Enqueue and tryDequeue")
    {
        REQUIRE(queue.enqueue(1));
        REQUIRE(queue.enqueue(2));
        REQUIRE(queue.size() == 2);
        REQUIRE_FALSE(queue.empty());

        int val = 0;
        REQUIRE(queue.tryDequeue(val));
        REQUIRE(val == 1);
        REQUIRE(queue.tryDequeue(val));
        REQUIRE(val == 2);
        REQUIRE(queue.empty());
        REQUIRE_FALSE(queue.tryDequeue(val));
    }

    SECTION("waitDequeue")
    {
        std::thread producer([&queue] {
            std::this_thread::sleep_for(50ms);
            queue.enqueue(42);
        });

        int val = 0;
        REQUIRE(queue.waitDequeue(val));
        REQUIRE(val == 42);
        producer.join();
    }

    SECTION("clear")
    {
        queue.enqueue(1);
        queue.enqueue(2);
        queue.clear();
        REQUIRE(queue.empty());
        REQUIRE(queue.size() == 0);
    }

    SECTION("bulk dequeue")
    {
        for (int i = 0; i < 5; ++i) {
            queue.enqueue(i);
        }

        std::vector<int> taken;
        REQUIRE(queue.tryDequeueBulk(std::back_inserter(taken), 3) == 3);
        REQUIRE(taken == std::vector<int>{0, 1, 2});
        REQUIRE(queue.tryDequeueBulk(std::back_inserter(taken), 10) == 2);
        REQUIRE(taken == std::vector<int>{0, 1, 2, 3, 4});
        REQUIRE(queue.tryDequeueBulk(std::back_inserter(taken), 10) == 0);
    }

    SECTION("growing keeps the order")
    {
        // More than the initial capacity, with the start of the buffer moving around
        for (int i = 0; i < 10; ++i) {
            queue.enqueue(i);
        }
        int val = 0;
        for (int i = 0; i < 5; ++i) {
            REQUIRE(queue.tryDequeue(val));
        }
        for (int i = 10; i < 100; ++i) {
            queue.enqueue(i);
        }
        for (int i = 5; i < 100; ++i) {
            REQUIRE(queue.tryDequeue(val));
            REQUIRE(val == i);
        }
    }
}

TEST_CASE("ConcurrentQueue with a std::stop_token", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<int> queue;
    std::stop_source source;

    SECTION("waitDequeue with token - stopped but not empty")
    {
        queue.enqueue(10);
        source.request_stop();
        int val = 0;
        REQUIRE(queue.waitDequeue(source.get_token(), val));
        REQUIRE(val == 10);
        REQUIRE_FALSE(queue.waitDequeue(source.get_token(), val));
    }

    SECTION("waitDequeue with token - stopped during wait")
    {
        std::thread stopper([&source] {
            std::this_thread::sleep_for(50ms);
            source.request_stop();
        });

        int val = 0;
        auto start = std::chrono::steady_clock::now();
        REQUIRE_FALSE(queue.waitDequeue(source.get_token(), val));
        auto end = std::chrono::steady_clock::now();
        REQUIRE(end - start >= 50ms);
        stopper.join();
    }

    SECTION("waitDequeue with token - already closed")
    {
        queue.enqueue(1);
        queue.close();
        int val = 0;
        REQUIRE_FALSE(queue.waitDequeue(source.get_token(), val));
    }

    SECTION("waitDequeue with token - closed during wait")
    {
        std::thread closer([&queue] {
            std::this_thread::sleep_for(50ms);
            queue.close();
        });

        int val = 0;
        auto start = std::chrono::steady_clock::now();
        REQUIRE_FALSE(queue.waitDequeue(source.get_token(), val));
        auto end = std::chrono::steady_clock::now();
        REQUIRE(end - start >= 50ms);
        closer.join();
    }

    SECTION("waitDequeue with token - both stopped and closed")
    {
        queue.close();
        source.request_stop();
        int val = 0;
        REQUIRE_FALSE(queue.waitDequeue(source.get_token(), val));
    }

    SECTION("waitDequeueBulk takes what is there, and returns 0 once stopped and empty")
    {
        queue.enqueue(1);
        queue.enqueue(2);
        std::vector<int> taken;
        REQUIRE(queue.waitDequeueBulk(source.get_token(), std::back_inserter(taken), 10) == 2);
        source.request_stop();
        REQUIRE(queue.waitDequeueBulk(source.get_token(), std::back_inserter(taken), 10) == 0);
        REQUIRE(taken == std::vector<int>{1, 2});
    }
}

TEST_CASE("ConcurrentQueue closing", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<int> queue;

    SECTION("close returns the remaining items")
    {
        queue.enqueue(1);
        queue.enqueue(2);
        auto remaining = queue.close();
        REQUIRE(remaining.size() == 2);
        REQUIRE(remaining[0] == 1);
        REQUIRE(remaining[1] == 2);
        REQUIRE(queue.isClosed());
    }

    SECTION("enqueue after close fails")
    {
        queue.close();
        REQUIRE_FALSE(queue.enqueue(1));
        REQUIRE(queue.empty());
    }

    SECTION("tryDequeue after close fails")
    {
        queue.enqueue(1);
        queue.close();
        int val = 0;
        REQUIRE_FALSE(queue.tryDequeue(val));
    }

    SECTION("waitDequeue after close fails")
    {
        std::thread closer([&queue]() {
            std::this_thread::sleep_for(50ms);
            queue.close();
        });

        int val = 0;
        REQUIRE_FALSE(queue.waitDequeue(val));
        closer.join();
    }
}

TEST_CASE("ConcurrentQueue concurrency", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<int> queue;
    std::stop_source stopSource;
    constexpr int numProducers = 4;
    constexpr int numConsumers = 4;
    constexpr int itemsPerProducer = 1000;
    std::atomic<int> totalConsumed{0};
    std::atomic<int> sumConsumed{0};

    std::vector<std::thread> producers;
    for (int i = 0; i < numProducers; ++i) {
        producers.emplace_back([&queue, i] {
            for (int j = 0; j < itemsPerProducer; ++j) {
                queue.enqueue(i * itemsPerProducer + j);
            }
        });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < numConsumers; ++i) {
        consumers.emplace_back([&queue, &totalConsumed, &sumConsumed, &stopSource, token = stopSource.get_token()] {
            int val = 0;

            while (queue.waitDequeue(token, val)) {
                sumConsumed += val;
                if (totalConsumed.fetch_add(1) + 1 == numProducers * itemsPerProducer) {
                    stopSource.request_stop();
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    REQUIRE(totalConsumed == numProducers * itemsPerProducer);

    int expected_sum = 0;
    for (int i = 0; i < numProducers * itemsPerProducer; ++i) {
        expected_sum += i;
    }

    REQUIRE(sumConsumed == expected_sum);
}

TEST_CASE("ConcurrentQueue destruction while waiting", "[concurrent][ConcurrentQueue]")
{
    // The destructor closes the queue, which wakes the consumer, and waits for it to leave.
    auto queue = std::make_unique<ConcurrentQueue<int>>();
    std::atomic<bool> thread_started{false};
    std::atomic<bool> wait_finished{false};

    // The thread uses the raw pointer: reading the unique_ptr while reset() writes it would itself be a data race.
    std::thread consumer([rawQueue = queue.get(), &thread_started, &wait_finished]() {
        thread_started = true;
        int val = 0;
        const bool result = rawQueue->waitDequeue(val);
        wait_finished = true;
        (void)result;
    });

    while (!thread_started) std::this_thread::yield();
    std::this_thread::sleep_for(10ms); // Give it some time to actually enter wait()

    queue.reset();

    consumer.join();
    REQUIRE(wait_finished);
}

TEST_CASE("ConcurrentQueue destruction with many waiters", "[concurrent][ConcurrentQueue]")
{
    constexpr int numWaiters = 50;
    auto queue = std::make_unique<ConcurrentQueue<int>>();
    std::vector<std::thread> waiters;
    std::atomic<int> completedWaiters{0};

    for (int i = 0; i < numWaiters; ++i) {
        // Raw pointer for the same reason as above: the unique_ptr itself is reset concurrently.
        waiters.emplace_back([rawQueue = queue.get(), &completedWaiters] {
            // Returns false when the queue is closed by its destructor
            if (int val = 0; !rawQueue->waitDequeue(val)) {
                ++completedWaiters;
            }
        });
    }

    // Give time for threads to block
    std::this_thread::sleep_for(50ms);

    queue.reset();

    for (auto& t : waiters) t.join();
    REQUIRE(completedWaiters == numWaiters);
}

TEST_CASE("ConcurrentQueue helper methods", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<int> queue;
    queue.enqueue(1);
    queue.enqueue(2);
    queue.enqueue(3);

    SECTION("contains")
    {
        REQUIRE(queue.contains([](int v) { return v == 2; }));
        REQUIRE_FALSE(queue.contains([](int v) { return v == 4; }));
    }

    SECTION("toList")
    {
        auto list = queue.toList<std::string>([](int v) { return std::to_string(v); });
        REQUIRE(list.size() == 3);
        REQUIRE(list[0] == "1");
        REQUIRE(list[1] == "2");
        REQUIRE(list[2] == "3");
    }
}

TEST_CASE("ConcurrentQueue enqueueFront", "[concurrent][ConcurrentQueue][enqueueFront]")
{
    ConcurrentQueue<int> queue;

    SECTION("EnqueueFront adds to the beginning")
    {
        queue.enqueue(1);
        queue.enqueue(2);

        REQUIRE(queue.enqueueFront(0));

        int val = 0;
        REQUIRE(queue.tryDequeue(val));
        REQUIRE(val == 0);
        REQUIRE(queue.tryDequeue(val));
        REQUIRE(val == 1);
        REQUIRE(queue.tryDequeue(val));
        REQUIRE(val == 2);
    }

    SECTION("EnqueueFront works on empty queue")
    {
        REQUIRE(queue.enqueueFront(10));
        int val = 0;
        REQUIRE(queue.tryDequeue(val));
        REQUIRE(val == 10);
    }
}
