#include "TestUtilities.h"

#include <vanilo/tasker/Tasker.h>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::waitFor;

namespace {
    // Waits until a stop is requested through the token (or a timeout), then reports it.
    bool waitForStop(const std::stop_token& token)
    {
        return waitFor([&token] { return token.stop_requested(); }, 5s);
    }
} // namespace

/// whenAll
/// ================================================================================================

TEST_CASE("whenAll goes on with the results of all the chains", "[whenAll]")
{
    const auto executor = ManualExecutor::create();

    auto handle = whenAll(Task::run(executor.get(), [] { return 6; }), Task::run(executor.get(), [] {
        return std::string{"x"};
    }).then(executor.get(), [](std::string text) {
        return text + "y";
    })).then(executor.get(), [](const int number, const std::string& text) {
        return text + std::to_string(number * 7);
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == "xy42");
}

TEST_CASE("whenAll keeps the non-void results, in order", "[whenAll]")
{
    const auto executor = ManualExecutor::create();
    bool voidRan = false;

    auto handle = whenAll(
                      Task::run(executor.get(), [&voidRan] { voidRan = true; }), Task::run(executor.get(), [] { return 2; }),
                      Task::run(executor.get(), [] {
        return 3.5;
    })).submit();
    STATIC_REQUIRE(std::is_same_v<decltype(handle), TaskHandle<std::tuple<int, double>>>);
    executor->runPending();

    REQUIRE(handle.get() == std::tuple{2, 3.5});
    REQUIRE(voidRan);
}

TEST_CASE("whenAll of void chains goes on with nothing", "[whenAll]")
{
    const auto executor = ManualExecutor::create();
    int runs = 0;

    auto handle = whenAll(Task::run(executor.get(), [&runs] { ++runs; }), Task::run(executor.get(), [&runs] { ++runs; }))
                      .then(executor.get(), [&runs] {
        return runs;
    }).submit();
    executor->runPending();

    REQUIRE(handle.get() == 2);
}

TEST_CASE("whenAll runs the chains at the same time", "[whenAll][pool]")
{
    const auto pool = ThreadPoolExecutor::create(4);
    const auto start = std::chrono::steady_clock::now();
    auto slow = [] {
        std::this_thread::sleep_for(100ms);
        return 1;
    };

    auto handle = whenAll(Task::run(pool.get(), slow), Task::run(pool.get(), slow), Task::run(pool.get(), slow)).submit();

    REQUIRE(handle.get() == std::tuple{1, 1, 1});
    CHECK((std::chrono::steady_clock::now() - start) < 250ms); // one after the other would take 300 ms
}

TEST_CASE("whenAll fails fast: the first error stops the other chains", "[whenAll][cancel]")
{
    const auto pool = ThreadPoolExecutor::create(2);
    std::atomic<bool> otherStopped{false};

    auto handle = whenAll(
                      Task::run(
                          pool.get(),
                          [&otherStopped](const std::stop_token& token) {
        otherStopped = waitForStop(token);
        return 1;
    }),
                      Task::run(pool.get(), []() -> int {
        throw std::runtime_error{"failed"};
    })).submit();

    REQUIRE_THROWS_AS(handle.get(), std::runtime_error);
    REQUIRE(waitFor([&otherStopped] { return otherStopped.load(); }));
}

TEST_CASE("The joined chains keep their own handlers", "[whenAll][exception]")
{
    const auto executor = ManualExecutor::create();
    bool observed = false;

    SECTION("an onError handler observes the error, the join still fails")
    {
        auto handle =
            whenAll(
                Task::run(executor.get(), []() -> int { throw std::runtime_error{"failed"}; }).onError([&observed](const std::exception&) {
            observed = true;
        }),
                Task::run(executor.get(), [] {
            return 2;
        })).submit();
        executor->runPending();

        REQUIRE(observed);
        REQUIRE_THROWS_AS(handle.get(), std::runtime_error);
    }

    SECTION("a recover makes the join succeed")
    {
        auto handle = whenAll(
                          Task::run(executor.get(), []() -> int { throw std::runtime_error{"failed"}; }).recover([](const std::exception&) {
            return 1;
        }),
                          Task::run(executor.get(), [] {
            return 2;
        })).submit();
        executor->runPending();

        REQUIRE(handle.get() == std::tuple{1, 2});
    }
}

TEST_CASE("A handler after whenAll receives the join's error", "[whenAll][exception]")
{
    const auto executor = ManualExecutor::create();
    std::string observed;

    auto handle =
        whenAll(Task::run(executor.get(), [] { return 1; }), Task::run(executor.get(), []() -> int { throw std::logic_error{"bad"}; }))
            .onError([&observed](const std::exception& ex) { observed = ex.what(); })
            .recover([](const std::exception&) {
        return std::tuple{0, 0};
    }).submit();
    executor->runPending();

    REQUIRE(observed == "bad");
    REQUIRE(handle.get() == std::tuple{0, 0});
}

TEST_CASE("Canceling a whenAll chain stops the joined chains", "[whenAll][cancel]")
{
    const auto pool = ThreadPoolExecutor::create(2);
    std::atomic<int> stopped{0};
    auto waiting = [&stopped](const std::stop_token& token) {
        if (waitForStop(token)) {
            ++stopped;
        }
    };

    auto handle = whenAll(Task::run(pool.get(), waiting), Task::run(pool.get(), waiting)).submit();
    std::this_thread::sleep_for(20ms);
    handle.cancel();

    REQUIRE(waitFor([&stopped] { return stopped == 2; }));
    handle.wait();
}

TEST_CASE("whenAll waits for delayed chains", "[whenAll][delay]")
{
    const auto executor = ManualExecutor::create();

    auto handle = whenAll(Task::run(executor.get(), 20ms, [] { return 1; }), Task::run(executor.get(), [] { return 2; })).submit();

    REQUIRE(waitFor([&] {
        executor->runPending();
        return handle.isReady();
    }));
    REQUIRE(handle.get() == std::tuple{1, 2});
}

TEST_CASE("whenAll of a range goes on with a vector of the results, in order", "[whenAll][range]")
{
    const auto pool = ThreadPoolExecutor::create(4);
    std::vector<TaskBuilder<int>> builders;
    for (int i = 0; i < 20; ++i) {
        builders.push_back(Task::run(pool.get(), [i] {
            std::this_thread::sleep_for(std::chrono::milliseconds{(20 - i) % 5});
            return i * i;
        }));
    }

    auto handle = whenAll(std::move(builders)).submit();
    const auto results = handle.get();

    REQUIRE(results.size() == 20);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(results[i] == i * i);
    }
}

TEST_CASE("whenAll of an empty range goes on at once", "[whenAll][range]")
{
    const auto executor = ManualExecutor::create();

    auto handle = whenAll(std::vector<TaskBuilder<int>>{}).submit();
    auto empty = whenAll(std::vector<TaskBuilder<void>>{}).then(executor.get(), [] { return 1; }).submit();
    executor->runPending();

    REQUIRE(handle.get().empty());
    REQUIRE(empty.get() == 1);
}

TEST_CASE("whenAll of a range of void chains goes on with nothing", "[whenAll][range]")
{
    const auto executor = ManualExecutor::create();
    int runs = 0;
    std::vector<TaskBuilder<void>> builders;
    builders.push_back(Task::run(executor.get(), [&runs] { ++runs; }));
    builders.push_back(Task::run(executor.get(), [&runs] { ++runs; }));

    auto handle = whenAll(std::move(builders)).submit();
    STATIC_REQUIRE(std::is_same_v<decltype(handle), TaskHandle<void>>);
    executor->runPending();

    REQUIRE_NOTHROW(handle.get());
    REQUIRE(runs == 2);
}

TEST_CASE("A joined chain whose executor is gone is canceled", "[whenAll][shutdown]")
{
    const auto executor = ManualExecutor::create();
    auto gone = ManualExecutor::create();
    auto builder = Task::run(gone.get(), [] { return 1; });
    gone.reset();

    auto handle = whenAll(Task::run(executor.get(), [] { return 2; }), std::move(builder)).submit();
    executor->runPending();

    REQUIRE_THROWS_AS(handle.get(), OperationCanceledException);
}

TEST_CASE("whenAll needs unused builders", "[whenAll][builder]")
{
    const auto executor = ManualExecutor::create();
    auto builder = Task::run(executor.get(), [] { return 1; });
    auto handle = std::move(builder).submit();

    REQUIRE_THROWS_AS(whenAll(std::move(builder), Task::run(executor.get(), [] { return 2; })), std::logic_error); // NOLINT
    executor->runPending();
    REQUIRE(handle.get() == 1);
}

/// whenAny
/// ================================================================================================

TEST_CASE("whenAny goes on with the first result", "[whenAny]")
{
    const auto pool = ThreadPoolExecutor::create(2);

    auto handle = whenAny(
                      Task::run(
                          pool.get(),
                          [](const std::stop_token& token) {
        waitForStop(token); // the slow one: stopped once the other wins
        return std::string{"slow"};
    }),
                      Task::run(pool.get(), [] {
        return 42;
    })).submit();
    const auto result = handle.get();

    REQUIRE(result.index() == 1);
    REQUIRE(std::get<1>(result) == 42);
}

TEST_CASE("whenAny takes the first success, not the first failure", "[whenAny]")
{
    const auto executor = ManualExecutor::create();

    auto handle =
        whenAny(Task::run(executor.get(), []() -> int { throw std::runtime_error{"first fails"}; }), Task::run(executor.get(), [] {
    })).submit();
    executor->runPending();
    const auto result = handle.get();

    REQUIRE(result.index() == 1);
    REQUIRE(std::holds_alternative<std::monostate>(result));
}

TEST_CASE("whenAny fails with the first error when all chains fail", "[whenAny][exception]")
{
    const auto executor = ManualExecutor::create();

    auto handle =
        whenAny(Task::run(executor.get(), []() -> int { throw std::logic_error{"first"}; }), Task::run(executor.get(), []() -> int {
        throw std::runtime_error{"second"};
    })).submit();
    executor->runPending();

    REQUIRE_THROWS_AS(handle.get(), std::logic_error);
}

TEST_CASE("whenAny of a range goes on with the index and the result of the first one", "[whenAny][range]")
{
    const auto executor = ManualExecutor::create();
    std::vector<TaskBuilder<int>> builders;
    builders.push_back(Task::run(executor.get(), []() -> int { throw std::runtime_error{"failed"}; }));
    builders.push_back(Task::run(executor.get(), [] { return 7; }));
    builders.push_back(Task::run(executor.get(), [] { return 8; }));

    auto handle = whenAny(std::move(builders)).submit();
    executor->runPending();

    REQUIRE(handle.get() == std::pair<std::size_t, int>{1, 7});
}

TEST_CASE("whenAny of a range of void chains goes on with the index", "[whenAny][range]")
{
    const auto executor = ManualExecutor::create();
    std::vector<TaskBuilder<void>> builders;
    builders.push_back(Task::run(executor.get(), [] {}));
    builders.push_back(Task::run(executor.get(), [] {}));

    auto handle = whenAny(std::move(builders)).submit();
    executor->runPending();

    REQUIRE(handle.get() == 0);
}

TEST_CASE("whenAny needs at least one chain", "[whenAny][range]")
{
    REQUIRE_THROWS_AS(whenAny(std::vector<TaskBuilder<int>>{}), std::invalid_argument);
}
