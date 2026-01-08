#include <vanilo/concurrent/ConcurrentQueue.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
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

        int val;
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

        int val;
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
}

TEST_CASE("ConcurrentQueue CancellationToken", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<int> queue;
    CancellationTokenSource cts;

    SECTION("waitDequeue with token - pre-canceled but not empty")
    {
        queue.enqueue(10);
        cts.cancel();
        int val;
        REQUIRE(queue.waitDequeue(cts.token(), val));
        REQUIRE(val == 10);
        REQUIRE_FALSE(queue.waitDequeue(cts.token(), val));
    }

    SECTION("waitDequeue with token - canceled during wait")
    {
        std::thread canceler([&cts] {
            std::this_thread::sleep_for(50ms);
            cts.cancel();
        });

        int val;
        auto start = std::chrono::steady_clock::now();
        REQUIRE_FALSE(queue.waitDequeue(cts.token(), val));
        auto end = std::chrono::steady_clock::now();
        REQUIRE(end - start >= 50ms);
        canceler.join();
    }

    SECTION("waitDequeue with token - already closed")
    {
        queue.enqueue(1);
        queue.close();
        int val;
        REQUIRE_FALSE(queue.waitDequeue(cts.token(), val));
    }

    SECTION("waitDequeue with token - closed during wait")
    {
        std::thread closer([&queue] {
            std::this_thread::sleep_for(50ms);
            queue.close();
        });

        int val;
        auto start = std::chrono::steady_clock::now();
        REQUIRE_FALSE(queue.waitDequeue(cts.token(), val));
        auto end = std::chrono::steady_clock::now();
        REQUIRE(end - start >= 50ms);
        closer.join();
    }

    SECTION("waitDequeue with token - both canceled and closed")
    {
        queue.close();
        cts.cancel();
        int val;
        REQUIRE_FALSE(queue.waitDequeue(cts.token(), val));
    }
}

TEST_CASE("ConcurrentQueue invalidation", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<int> queue;

    SECTION("invalidate returns remaining items")
    {
        queue.enqueue(1);
        queue.enqueue(2);
        auto remaining = queue.close();
        REQUIRE(remaining.size() == 2);
        REQUIRE(remaining[0] == 1);
        REQUIRE(remaining[1] == 2);
        REQUIRE(queue.isClosed());
    }

    SECTION("enqueue after invalidate fails")
    {
        queue.close();
        REQUIRE_FALSE(queue.enqueue(1));
        REQUIRE(queue.empty());
    }

    SECTION("tryDequeue after invalidate fails")
    {
        queue.enqueue(1);
        queue.close();
        int val;
        REQUIRE_FALSE(queue.tryDequeue(val));
    }

    SECTION("waitDequeue after invalidate fails")
    {
        std::thread invalidator([&queue]() {
            std::this_thread::sleep_for(50ms);
            queue.close();
        });

        int val;
        REQUIRE_FALSE(queue.waitDequeue(val));
        invalidator.join();
    }
}

TEST_CASE("ConcurrentQueue concurrency", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<int> queue;
    CancellationTokenSource tokenSource;
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
        consumers.emplace_back([&queue, &totalConsumed, &sumConsumed, &tokenSource, token = tokenSource.token()] {
            int val;

            while (queue.waitDequeue(token, val)) {
                sumConsumed += val;
                totalConsumed.fetch_add(1);

                if (totalConsumed.load(std::memory_order_acquire) == numProducers * itemsPerProducer) {
                    tokenSource.cancel();
                }
            }
        });
    }

    for (auto& t : producers) t.join();

    // After producers are done, we might need to invalidate to wake up consumers if they are stuck in waitDequeue
    // but here they should finish because we know how many items to expect.
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
    // This test is tricky because it involves intentional race conditions.
    // The goal is to see if it crashes.
    auto* queue = new ConcurrentQueue<int>();
    std::atomic<bool> thread_started{false};
    std::atomic<bool> wait_finished{false};

    std::thread consumer([queue, &thread_started, &wait_finished]() {
        thread_started = true;
        int val;
        const bool result = queue->waitDequeue(val);
        wait_finished = true;
        (void)result;
    });

    while (!thread_started) std::this_thread::yield();
    std::this_thread::sleep_for(10ms); // Give it some time to actually enter wait()

    delete queue; // Should call close() which should wake up the consumer

    consumer.join();
    REQUIRE(wait_finished);
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
