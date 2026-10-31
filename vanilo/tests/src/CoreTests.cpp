#include <vanilo/concurrent/CancellationToken.h>
#include <vanilo/concurrent/ConcurrentQueue.h>
#include <vanilo/core/Binder.h>
#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/Tasker.h>

#include "TestUtilities.h"

#include <catch2/catch_all.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace vanilo::concurrent;
using namespace vanilo::core;
using namespace vanilo::tasker;
using vanilo::tests::StandardErrorCapture;

/// LifeCycleGuard::Lock
/// ================================================================================================

TEST_CASE("LifeCycleGuard::Lock can be moved", "[LifeCycleGuard][lock]")
{
    LifeCycleGuard guard;

    SECTION("move construction transfers the reference")
    {
        auto first = guard.acquire();
        auto second = std::move(first);
        REQUIRE_FALSE(first); // NOLINT(bugprone-use-after-move): checking the moved-from state is the point
        REQUIRE(second);
    }

    SECTION("move assignment releases the old reference and takes the new one")
    {
        LifeCycleGuard other;
        auto lock = guard.acquire();
        auto otherLock = other.acquire();

        lock = std::move(otherLock);
        REQUIRE(lock);
        REQUIRE_FALSE(otherLock); // NOLINT(bugprone-use-after-move)

        // guard's reference was released, so terminate() returns immediately; other's is still held.
        guard.terminate();
        lock = LifeCycleGuard::Lock{};
        other.terminate();
    }

    SECTION("self move assignment keeps the reference")
    {
        auto lock = guard.acquire();
        auto& alias = lock;
        lock = std::move(alias);
        REQUIRE(lock);
    }

    SECTION("a terminated guard can't be acquired")
    {
        guard.terminate();
        REQUIRE_FALSE(guard.acquire());
        REQUIRE_FALSE(guard.tryIncrement());
    }
}

/// Binder
/// ================================================================================================

TEST_CASE("A bound function object can be copied", "[binder]")
{
    auto bound = binder::bind([](const int a, const int b) { return a + b; }, 1, 2);
    auto copy = bound;

    REQUIRE(bound() == 3);
    REQUIRE(copy() == 3);
}

TEST_CASE("bind forwards call-time arguments after the bound ones", "[binder]")
{
    auto bound = binder::bind([](const int a, const int b, const int c) { return a * 100 + b * 10 + c; }, 1);
    REQUIRE(bound(2, 3) == 123);
}

/// Tracer
/// ================================================================================================

TEST_CASE("Tracer writes the source location and the formatted message to stderr", "[tracer]")
{
    const StandardErrorCapture capture;

    Tracer{"file.cpp", 42}("value %d", 7);
    Tracer{"other.cpp", 3}("no arguments");

    REQUIRE(capture.text() == "file.cpp(42): value 7\nother.cpp(3): no arguments\n");
}

TEST_CASE("Tracer truncates long output instead of overflowing its buffer", "[tracer]")
{
    const StandardErrorCapture capture;
    const std::string longName(300, 'x');
    const std::string longText(300, 'y');

    Tracer{longName.c_str(), 1}("%s", longText.c_str()); // the prefix alone doesn't fit
    Tracer{"short.cpp", 1}("%s", longText.c_str());       // the message doesn't fit

    const auto text = capture.text();
    const auto firstLine = text.substr(0, text.find('\n'));
    REQUIRE(firstLine.size() == 255);
    REQUIRE(text.find("short.cpp(1): yyy") != std::string::npos);
}

/// ConcurrentQueue
/// ================================================================================================

TEST_CASE("A closed ConcurrentQueue rejects new elements", "[concurrent][ConcurrentQueue]")
{
    ConcurrentQueue<std::string> queue;
    queue.enqueue("kept");

    const auto remaining = queue.close();
    REQUIRE(remaining == std::vector<std::string>{"kept"});
    REQUIRE(queue.isClosed());

    std::string value = "unchanged";
    const std::string late = "late";
    REQUIRE_FALSE(queue.enqueue("late"));            // constructed in place
    REQUIRE_FALSE(queue.enqueue(late));              // copied
    REQUIRE_FALSE(queue.enqueue(std::string{"late"})); // moved
    REQUIRE_FALSE(queue.enqueueFront(late));
    REQUIRE_FALSE(queue.enqueueFront(std::string{"late"}));
    REQUIRE_FALSE(queue.tryDequeue(value));
    REQUIRE(value == "unchanged");
    REQUIRE(queue.close().empty()); // closing twice is harmless
}

/// CancellationToken
/// ================================================================================================

TEST_CASE("CancellationTokenSource reports its own cancellation", "[token]")
{
    const CancellationTokenSource source;
    REQUIRE_FALSE(source.isCancellationRequested());
    source.cancel();
    REQUIRE(source.isCancellationRequested());
    source.cancel(); // canceling twice is harmless
    REQUIRE(source.token().isCancellationRequested());
}

TEST_CASE("A Subscription can be move-assigned", "[token][subscription][move]")
{
    const CancellationTokenSource source;
    int first = 0;
    int second = 0;

    auto subscription = source.token().subscribe([&first] { ++first; });
    subscription = source.token().subscribe([&second] { ++second; }); // the first one is unsubscribed

    source.cancel();

    REQUIRE(first == 0);
    REQUIRE(second == 1);
}

TEST_CASE("A throwing cancellation callback doesn't stop the others", "[token][subscribe]")
{
    const CancellationTokenSource source;
    int called = 0;

    auto throwsStandard = source.token().subscribe([&called] {
        ++called;
        throw std::runtime_error{"callback failed"};
    });
    auto throwsOther = source.token().subscribe([&called] {
        ++called;
        throw 42; // NOLINT
    });
    auto succeeds = source.token().subscribe([&called] { ++called; });

    REQUIRE_NOTHROW(source.cancel());
    REQUIRE(called == 3);
}

TEST_CASE("Subscription slots are reused as subscriptions come and go", "[token][subscribe]")
{
    // More subscriptions than the slot scanner looks at in one go, with gaps all over the list.
    const CancellationTokenSource source;
    std::vector<int> calls(80, 0);
    std::vector<std::optional<CancellationToken::Subscription>> subscriptions(80);

    for (size_t i = 0; i < 40; ++i) {
        subscriptions[i] = source.token().subscribe([&calls, i] { ++calls[i]; });
    }
    for (size_t i = 0; i < 40; i += 3) {
        subscriptions[i].reset(); // frees a slot
    }
    for (size_t i = 40; i < 80; ++i) {
        subscriptions[i] = source.token().subscribe([&calls, i] { ++calls[i]; });
    }

    source.cancel();

    for (size_t i = 0; i < 80; ++i) {
        const int expected = (i < 40 && i % 3 == 0) ? 0 : 1;
        INFO("subscription " << i);
        REQUIRE(calls[i] == expected);
    }
}

/// Executors
/// ================================================================================================

TEST_CASE("ThreadPoolExecutor reports its threads", "[executor][pool]")
{
    const auto executor = ThreadPoolExecutor::create();

    REQUIRE(executor->threadCount() == ThreadPoolExecutor::DefaultThreadNumber);

    const auto ids = executor->threadIds();
    REQUIRE(ids.size() == executor->threadCount());
    REQUIRE(executor->containsThread(ids.front()));
    REQUIRE_FALSE(executor->containsThread(std::this_thread::get_id()));
}

TEST_CASE("ThreadPoolExecutor::count() reports the queued tasks", "[executor][pool]")
{
    const auto executor = ThreadPoolExecutor::create(1);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> blocking{false};

    Task::run(executor.get(), [released, &blocking] {
        blocking = true;
        released.wait();
    });
    REQUIRE(vanilo::tests::waitFor([&blocking] { return blocking.load(); }));

    Task::run(executor.get(), [] {});
    Task::run(executor.get(), [] {});
    REQUIRE(executor->count() == 2);

    release.set_value();
    REQUIRE(vanilo::tests::waitFor([&executor] { return executor->count() == 0; }));
}

TEST_CASE("LocalThreadExecutor::process(0) processes nothing", "[executor][local]")
{
    const auto executor = LocalThreadExecutor::create();
    bool ran = false;
    Task::run(executor.get(), [&ran] { ran = true; });

    REQUIRE(executor->process(0) == 1);
    REQUIRE_FALSE(ran);
}

TEST_CASE("A chain can move between executors", "[executor][chain]")
{
    const auto local = LocalThreadExecutor::create();
    const auto pool = ThreadPoolExecutor::create(2);
    std::atomic<bool> ranOnPool{false};

    auto future = Task::run(local.get(), [] { return 2; })
                      .then(pool.get(),
                            [&pool, &ranOnPool](const int value) {
                                ranOnPool = pool->containsThread(std::this_thread::get_id());
                                return value * 10;
                            })
                      .then(local.get(), [](const int value) { return value + 1; })
                      .getFuture();

    REQUIRE(vanilo::tests::waitFor([&] {
        local->process(10);
        return future.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
    }));
    REQUIRE(future.get() == 21);
    REQUIRE(ranOnPool);
}

TEST_CASE("A freed subscription slot is reused even outside the scanned window", "[token][subscribe]")
{
    // New subscriptions are appended until a slot is free; the scanner first looks at a window of
    // slots after the last insertion, then falls back to scanning the whole list.
    const CancellationTokenSource source;
    std::vector<int> calls(41, 0);
    std::vector<std::optional<CancellationToken::Subscription>> subscriptions(41);

    for (size_t i = 0; i < 40; ++i) {
        subscriptions[i] = source.token().subscribe([&calls, i] { ++calls[i]; });
    }
    subscriptions[30].reset(); // the only free slot, far from the start of the list
    subscriptions[40] = source.token().subscribe([&calls] { ++calls[40]; });

    source.cancel();

    for (size_t i = 0; i < 41; ++i) {
        INFO("subscription " << i);
        REQUIRE(calls[i] == (i == 30 ? 0 : 1));
    }
}
