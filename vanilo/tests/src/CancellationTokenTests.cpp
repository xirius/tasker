#include <vanilo/concurrent/CancellationToken.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace vanilo::concurrent;
using namespace std::chrono_literals;

TEST_CASE("Default tokens (none) compare equal", "[token][equality]")
{
    // Default-constructed tokens are never-cancelable and compare equal (both have null impl)
    CancellationToken none1;
    CancellationToken none2;
    REQUIRE(none1 == none2);

    // Tokens from the same source compare equal
    const CancellationTokenSource sourceA;
    auto tokenA1 = sourceA.token();
    auto tokenA2 = sourceA.token();
    REQUIRE(tokenA1 == tokenA2);
}

TEST_CASE("Tokens from different sources compare not equal", "[token][equality]")
{
    const CancellationTokenSource sourceA;
    auto tokenA = sourceA.token();
    const CancellationTokenSource sourceB;
    auto tokenB = sourceB.token();
    REQUIRE(tokenA != tokenB);
}

TEST_CASE("Token copyability preserves identity", "[token][copy]")
{
    const CancellationTokenSource source;
    auto token1 = source.token();
    CancellationToken token2 = token1;
    REQUIRE(token1 == token2);
}

TEST_CASE("none() token never cancels and subscribe is no-op", "[token][none]")
{
    const auto token = CancellationToken::none();
    REQUIRE_FALSE(token.isCancellationRequested());

    std::atomic<int> called{0};
    {
        const auto subscription = token.subscribe([&called] { called.fetch_add(1, std::memory_order_relaxed); });
        (void)subscription;
    }

    // There is no way to cancel a none() token; callback must not be called
    std::this_thread::sleep_for(10ms);
    REQUIRE(called.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("isCancellationRequested reflects cancel()", "[token][cancel]")
{
    const CancellationTokenSource source;
    const auto token = source.token();

    REQUIRE_FALSE(token.isCancellationRequested());
    REQUIRE_NOTHROW(token.throwIfCancellationRequested());
}

TEST_CASE("throwIfCancellationRequested throws after cancel", "[token][cancel]")
{
    const CancellationTokenSource source;
    const auto token = source.token();

    REQUIRE_FALSE(token.isCancellationRequested());
    // Before cancel, no throw
    REQUIRE_NOTHROW(token.throwIfCancellationRequested());

    source.cancel();

    REQUIRE(token.isCancellationRequested());
    REQUIRE_THROWS_AS(token.throwIfCancellationRequested(), OperationCanceledException);
}

TEST_CASE("subscribe before cancel: callback invoked once on cancel", "[token][subscribe]")
{
    CancellationTokenSource source;
    auto token = source.token();

    std::atomic<int> count{0};
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;

    auto subscription = token.subscribe([&count, &mutex, &cv, &done] {
        count.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard lockGuard{mutex};
        done = true;
        cv.notify_one();
    });

    source.cancel();

    std::unique_lock lock{mutex};
    REQUIRE(cv.wait_for(lock, 500ms, [&] { return done; }));
    REQUIRE(count.load(std::memory_order_relaxed) == 1);

    // Idempotent: subsequent cancel has no effect
    source.cancel();
    REQUIRE(count.load(std::memory_order_relaxed) == 1);

    (void)subscription;
}

TEST_CASE("subscribe after cancel: callback invoked immediately once", "[token][subscribe]")
{
    const CancellationTokenSource source;
    const auto token = source.token();
    source.cancel();

    std::atomic<int> count{0};
    const auto subscription = token.subscribe([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    (void)subscription;
    REQUIRE(count.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("unsubscribe before cancel suppresses callback", "[token][unsubscribe]")
{
    const CancellationTokenSource source;
    const auto token = source.token();
    std::atomic<int> count{0};
    {
        auto subscription = token.subscribe([&count] { count.fetch_add(1, std::memory_order_relaxed); });
        subscription.unsubscribe();
    } // moved-from/destroyed; already unsubscribed

    source.cancel();
    std::this_thread::sleep_for(10ms);
    REQUIRE(count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("Subscription move semantics: moved-to controls unsubscription", "[token][subscription][move]")
{
    const CancellationTokenSource source;
    const auto token = source.token();
    std::atomic<int> count{0};

    CancellationToken::Subscription sub1 = token.subscribe([&count] { count.fetch_add(1, std::memory_order_relaxed); });
    CancellationToken::Subscription sub2 = std::move(sub1);
    // sub1 is now moved-from; use sub2 to unsubscribe
    sub2.unsubscribe();

    source.cancel();
    std::this_thread::sleep_for(10ms);
    REQUIRE(count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("At-most-once delivery under concurrent subscribe/cancel", "[token][concurrency]")
{
    for (int round = 0; round < 5; ++round) {
        CancellationTokenSource source;
        auto token = source.token();
        constexpr int N = 200;
        std::atomic<int> hits{0};
        std::vector<CancellationToken::Subscription> subscriptions;
        subscriptions.reserve(N);

        std::thread producer([&subscriptions, &token, &hits] {
            for (int i = 0; i < N; ++i) {
                subscriptions.emplace_back(token.subscribe([&hits] { hits.fetch_add(1, std::memory_order_relaxed); }));
                // create slight variability
                if (i % 7 == 0)
                    std::this_thread::yield();
            }
        });

        // Race: cancel at some point during/after registrations
        std::thread canceller([&source] {
            std::this_thread::sleep_for(1ms);
            source.cancel();
        });

        producer.join();
        canceller.join();

        // Allow callbacks to run
        std::this_thread::sleep_for(10ms);

        // Every subscription should have been invoked exactly once
        REQUIRE(hits.load(std::memory_order_relaxed) == N);
    }
}

TEST_CASE("Unsubscribed fraction is not notified on cancel", "[token][unsubscribe][concurrency]")
{
    const CancellationTokenSource source;
    const auto token = source.token();
    constexpr int N = 200;
    std::atomic<int> hits{0};
    std::vector<CancellationToken::Subscription> subscriptions;
    subscriptions.reserve(N);

    for (int i = 0; i < N; ++i) {
        subscriptions.emplace_back(token.subscribe([&hits] { hits.fetch_add(1, std::memory_order_relaxed); }));
    }

    // Unsubscribe half before cancel
    for (int i = 0; i < N; i += 2) {
        subscriptions[i].unsubscribe();
    }

    source.cancel();
    std::this_thread::sleep_for(10ms);
    REQUIRE(hits.load(std::memory_order_relaxed) == N / 2);
}