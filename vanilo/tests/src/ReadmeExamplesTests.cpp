#include <vanilo/tasker/Tasker.h>

#include "TestUtilities.h"

#include <catch2/catch_all.hpp>

#include <atomic>
#include <string>
#include <vector>

// The examples of README.md, so they keep compiling and doing what the README says.

using namespace vanilo::concurrent;
using namespace vanilo::tasker;
using namespace std::chrono_literals;
using vanilo::tests::UnhandledExceptions;
using vanilo::tests::waitFor;

TEST_CASE("README: the introduction example", "[readme]")
{
    auto pool = ThreadPoolExecutor::create();

    auto answer = Task::run(pool.get(), [] { return 6; }).then(pool.get(), [](int value) { return value * 7; }).getFuture();

    REQUIRE(answer.get() == 42);
}

TEST_CASE("README: executors", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(4);
    pool->resize(8).wait();
    REQUIRE(pool->threadCount() == 8);

    auto local = LocalThreadExecutor::create();
    REQUIRE(local->process(10) == 0);
}

TEST_CASE("README: an event loop on a LocalThreadExecutor", "[readme]")
{
    auto local = LocalThreadExecutor::create();
    CancellationTokenSource stop;
    std::atomic<bool> ran{false};

    std::thread producer{[&] {
        Task::run(local.get(), [&ran, &stop] {
            ran = true;
            stop.cancel(); // ends the loop below
        });
    }};

    local->process(stop.token());
    producer.join();

    REQUIRE(ran);
}

TEST_CASE("README: arguments are copied, std::ref changes in place", "[readme]")
{
    auto local = LocalThreadExecutor::create();
    int counter = 0;

    Task::run(local.get(), [](int& value, int step) { value += step; }, std::ref(counter), 5);
    local->process(1);

    REQUIRE(counter == 5);
}

namespace {
    struct Thermostat
    {
        void setTarget(double celsius)
        {
            target = celsius;
        }

        double target = 0;
    };
} // namespace

TEST_CASE("README: member functions on an existing object", "[readme]")
{
    auto local = LocalThreadExecutor::create();
    Thermostat thermostat;

    Task::run(local.get(), &Thermostat::setTarget, &thermostat, 21.5);
    local->process(1);

    REQUIRE(thermostat.target == 21.5);
}

TEST_CASE("README: a chain across executors", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    auto local = LocalThreadExecutor::create();
    auto readSensor = [] { return 22.5; };

    auto future = Task::run(pool.get(), readSensor).then(local.get(), [](double celsius) { return celsius > 21; }).getFuture();

    REQUIRE(waitFor([&] {
        local->process(1);
        return future.wait_for(0ms) == std::future_status::ready;
    }));
    REQUIRE(future.get());
}

TEST_CASE("README: one handler for the errors of a chain", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(2);
    std::atomic<bool> logged{false};
    auto download = []() -> std::string { throw std::runtime_error{"offline"}; };

    Task::run(pool.get(), download)
        .then(pool.get(), [](const std::string&) {})
        .onException([&logged](const std::exception& ex) { logged = std::string{ex.what()} == "offline"; });

    REQUIRE(waitFor([&logged] { return logged.load(); }));
}

namespace {
    struct Device
    {
        void connect()
        {
            throw std::runtime_error{"no answer"};
        }

        void markOffline(const std::string& reason)
        {
            offlineReason = reason;
        }

        std::string offlineReason;
    };
} // namespace

TEST_CASE("README: a handler on another executor, with the token and the task's arguments", "[readme]")
{
    auto local = LocalThreadExecutor::create();
    auto pool = ThreadPoolExecutor::create(1);
    CancellationTokenSource source;
    auto token = source.token();
    Device device;
    std::atomic<bool> handled{false};

    Task::run(local.get(), token, [](const CancellationToken&, Device& target) { target.connect(); }, std::ref(device))
        .onException(pool.get(), [&handled](const std::exception& ex, const CancellationToken&, Device& target) {
            target.markOffline(ex.what());
            handled = true;
        });

    local->process(1);
    REQUIRE(waitFor([&handled] { return handled.load(); }));
    REQUIRE(device.offlineReason == "no answer");
}

TEST_CASE("README: a custom unhandled-exception handler", "[readme]")
{
    const UnhandledExceptions unhandled; // installs a recording handler like the README's
    auto local = LocalThreadExecutor::create();

    Task::run(local.get(), [] { throw std::runtime_error{"nobody handles this"}; });
    local->process(1);

    REQUIRE(unhandled.messages() == std::vector<std::string>{"nobody handles this"});
}

TEST_CASE("README: a task that stops halfway when canceled", "[readme]")
{
    auto pool = ThreadPoolExecutor::create(1);
    CancellationTokenSource source;
    std::atomic<int> processed{0};
    const std::vector<int> chunks(1000, 1);

    auto future = Task::run(pool.get(), source.token(), [&chunks, &processed, &source](const CancellationToken& token) {
                      for (const auto& chunk : chunks) {
                          token.throwIfCancellationRequested();
                          processed += chunk;
                          if (processed == 10) {
                              source.cancel();
                          }
                      }
                  }).getFuture();

    REQUIRE_THROWS_AS(future.get(), OperationCanceledException);
    REQUIRE(processed == 10);
}

TEST_CASE("README: reacting to a cancellation", "[readme]")
{
    CancellationTokenSource source;
    bool aborted = false;

    auto subscription = source.token().subscribe([&aborted] { aborted = true; });
    source.cancel();

    REQUIRE(aborted);
    REQUIRE_FALSE(CancellationToken::none().isCancellationRequested());
}

TEST_CASE("README: delayed and periodic tasks", "[readme]")
{
    auto local = LocalThreadExecutor::create();
    CancellationTokenSource stop;
    std::atomic<int> once{0};
    std::atomic<int> repeated{0};

    Task::run(local.get(), stop.token(), 5ms, [&once] { ++once; });
    Task::run(local.get(), stop.token(), 0ms, 5ms, [&repeated] { ++repeated; }).onException([](const std::exception&) {});

    REQUIRE(waitFor([&] {
        local->process(10);
        return once == 1 && repeated >= 2;
    }));
    stop.cancel();
}
