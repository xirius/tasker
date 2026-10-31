#include "TestUtilities.h"

#include <vanilo/concurrent/ConcurrentQueue.h>
#include <vanilo/core/Tracer.h>
#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace vanilo::concurrent;
using namespace vanilo::core;
using namespace vanilo::tasker;
using vanilo::tests::StandardErrorCapture;

/// Bound arguments
/// ================================================================================================

TEST_CASE("Bound arguments are copied into the task", "[binder]")
{
    const auto executor = ManualExecutor::create();
    std::string text = "before";

    auto handle = Task::run(executor.get(), [](const std::string& value) { return value; }, text).submit();
    text = "after";
    executor->runPending();

    REQUIRE(handle.get() == "before");
}

TEST_CASE("The previous result comes after the bound arguments", "[binder]")
{
    const auto executor = ManualExecutor::create();

    auto handle = Task::run(executor.get(), [] { return 3; })
                      .then(executor.get(), [](const int a, const int b, const int c) { return a * 100 + b * 10 + c; }, 1, 2)
                      .submit();
    executor->runPending();

    REQUIRE(handle.get() == 123);
}

TEST_CASE("Move-only functions, arguments and results work for one-shot tasks", "[binder]")
{
    const auto executor = ManualExecutor::create();
    auto owned = std::make_unique<int>(20);

    auto handle = Task::run(executor.get(), [owned = std::move(owned)] { return std::make_unique<int>(*owned + 1); })
                      .then(
                          executor.get(), [](std::unique_ptr<int>& value, std::unique_ptr<int> input) { return *value + *input; },
                          std::make_unique<int>(21))
                      .submit();
    executor->runPending();

    REQUIRE(handle.get() == 42);
}

TEST_CASE("Generic lambdas and overloaded function objects can be tasks", "[binder]")
{
    struct Overloaded
    {
        int operator()(const int value) const
        {
            return value * 2;
        }

        std::string operator()(const std::string& value) const
        {
            return value + value;
        }
    };

    const auto executor = ManualExecutor::create();

    auto generic = Task::run(executor.get(), [](auto value) { return value + 1; }, 41).submit();
    auto overloadedInt = Task::run(executor.get(), [] { return 21; }).then(executor.get(), Overloaded{}).submit();
    auto overloadedText = Task::run(executor.get(), [] { return std::string{"ab"}; }).then(executor.get(), Overloaded{}).submit();
    executor->runPending();

    REQUIRE(generic.get() == 42);
    REQUIRE(overloadedInt.get() == 42);
    REQUIRE(overloadedText.get() == "abab");
}

TEST_CASE("A task returning a reference passes a copy on", "[binder]")
{
    const auto executor = ManualExecutor::create();
    int value = 1;

    auto handle = Task::run(executor.get(), [&value]() -> int& { return value; }).submit();
    executor->runPending();
    value = 2;

    REQUIRE(handle.get() == 1);
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

TEST_CASE("Tracer writes where it was created by default", "[tracer]")
{
    const StandardErrorCapture capture;

    const auto line = __LINE__ + 1;
    Tracer{}("here");

    REQUIRE_THAT(capture.text(), Catch::Matchers::ContainsSubstring("CoreTests.cpp(" + std::to_string(line) + "): here"));
}

TEST_CASE("Tracer truncates long output instead of overflowing its buffer", "[tracer]")
{
    const StandardErrorCapture capture;
    const std::string longName(300, 'x');
    const std::string longText(300, 'y');

    Tracer{longName.c_str(), 1}("%s", longText.c_str()); // the prefix alone doesn't fit
    Tracer{"short.cpp", 1}("%s", longText.c_str());      // the message doesn't fit

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
    std::string late = "late";
    REQUIRE_FALSE(queue.enqueue("late"));              // constructed in place
    REQUIRE_FALSE(queue.enqueue(std::as_const(late))); // copied
    REQUIRE_FALSE(queue.enqueue(std::move(late)));     // moved: left untouched when refused
    REQUIRE(late == "late");                           // NOLINT(bugprone-use-after-move)
    REQUIRE_FALSE(queue.enqueueFront(std::as_const(late)));
    REQUIRE_FALSE(queue.enqueueFront(std::string{"late"}));
    REQUIRE_FALSE(queue.tryDequeue(value));
    REQUIRE(value == "unchanged");
    REQUIRE(queue.close().empty()); // closing twice is harmless
}

/// Executors
/// ================================================================================================

TEST_CASE("ThreadPoolExecutor reports its threads", "[executor][pool]")
{
    const auto executor = ThreadPoolExecutor::create();

    REQUIRE(executor->threadCount() == ThreadPoolExecutor::defaultThreadCount());

    const auto ids = executor->threadIds();
    REQUIRE(ids.size() == executor->threadCount());
    REQUIRE(executor->containsThread(ids.front()));
    REQUIRE_FALSE(executor->containsThread(std::this_thread::get_id()));
}

TEST_CASE("ThreadPoolExecutor::pendingCount() reports the queued tasks", "[executor][pool]")
{
    const auto executor = ThreadPoolExecutor::create(1);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> blocking{false};

    Task::post(executor.get(), [released, &blocking] {
        blocking = true;
        released.wait();
    });
    REQUIRE(vanilo::tests::waitFor([&blocking] { return blocking.load(); }));

    Task::post(executor.get(), [] {});
    Task::post(executor.get(), [] {});
    REQUIRE(executor->pendingCount() == 2);

    release.set_value();
    REQUIRE(vanilo::tests::waitFor([&executor] { return executor->pendingCount() == 0; }));
}

TEST_CASE("ManualExecutor::runPending(0) runs nothing", "[executor][manual]")
{
    const auto executor = ManualExecutor::create();
    bool ran = false;
    Task::post(executor.get(), [&ran] { ran = true; });

    REQUIRE(executor->runPending(0) == 1);
    REQUIRE_FALSE(ran);
}

TEST_CASE("ManualExecutor::runPending() runs the tasks queued by the tasks it runs", "[executor][manual]")
{
    const auto executor = ManualExecutor::create();
    int runs = 0;

    Task::post(executor.get(), [&executor, &runs] {
        ++runs;
        Task::post(executor.get(), [&runs] { ++runs; });
    });

    REQUIRE(executor->runPending() == 0);
    REQUIRE(runs == 2);
}

TEST_CASE("A chain can move between executors", "[executor][chain]")
{
    const auto manual = ManualExecutor::create();
    const auto pool = ThreadPoolExecutor::create(2);
    std::atomic<bool> ranOnPool{false};

    auto handle = Task::run(manual.get(), [] { return 2; })
                      .then(pool.get(), [&pool, &ranOnPool](const int value) {
        ranOnPool = pool->containsThread(std::this_thread::get_id());
        return value * 10;
    }).then(manual.get(), [](const int value) {
        return value + 1;
    }).submit();

    REQUIRE(vanilo::tests::waitFor([&] {
        manual->runPending(10);
        return handle.isReady();
    }));
    REQUIRE(handle.get() == 21);
    REQUIRE(ranOnPool);
}

TEST_CASE("A Task object submitted to an executor runs there", "[executor][submit]")
{
    std::atomic<int> runs{0};
    std::atomic<bool> canceled{false};
    const auto executor = ManualExecutor::create();

    executor->submit(std::make_unique<vanilo::tests::RecordingTask>(runs, canceled));
    executor->submit(nullptr); // ignored
    REQUIRE(executor->pendingCount() == 1);
    executor->runPending();

    REQUIRE(runs == 1);
    REQUIRE_FALSE(canceled);
}
